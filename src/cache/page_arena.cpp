#include "cache/page_arena.h"

#include "util/check.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tinydb::cache {
namespace {

auto PageBytesFor(std::size_t pages) noexcept -> std::size_t { return pages * PAGE_SIZE; }

}  // namespace
struct PageArena::Impl final {
  static constexpr auto MINIMUM_SLAB_PAGES = std::size_t{256};
  static constexpr auto MAXIMUM_SLAB_PAGES = std::size_t{1024};

  struct FreeExtent final {
    std::size_t offset;
    std::size_t count;
  };

  struct Slab final {
    explicit Slab(std::size_t page_count) : count(page_count) {
      if (count == 0 || count > std::numeric_limits<std::size_t>::max() / PAGE_SIZE) {
        throw std::length_error("direct page-arena slab is too large");
      }
      // Reserve the worst-case number of free extents now, because Lease
      // destruction must return a page without allocating.
      free_extents.reserve(count);
      free_extents.push_back(FreeExtent{.offset = 0, .count = count});
      const auto byte_count = PageBytesFor(count);
      // MAP_SHARED permits MADV_REMOVE during reclamation; the anonymous pages
      // consume physical memory only after a caller writes them.
      auto *const mapping = ::mmap(nullptr, byte_count, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      if (mapping == MAP_FAILED) {
        throw std::bad_alloc{};
      }
      if (reinterpret_cast<std::uintptr_t>(mapping) % PAGE_SIZE != 0) {
        static_cast<void>(::munmap(mapping, byte_count));
        throw std::bad_alloc{};
      }
      pages = static_cast<PageBytes *>(mapping);
      // Begin the PageBytes array lifetime without touching the lazy mapping.
      std::uninitialized_default_construct_n(pages, count);
    }

    Slab(const Slab &) = delete;
    auto operator=(const Slab &) -> Slab & = delete;

    ~Slab() {
      if (pages != nullptr) {
        std::destroy_n(pages, count);
        TINYDB_CHECK(::munmap(pages, PageBytesFor(count)) == 0, "could not unmap direct page arena");
      }
    }

    PageBytes *pages{nullptr};
    std::size_t count{0};
    std::vector<FreeExtent> free_extents;
  };

  struct Allocation final {
    std::size_t slab_index;
    std::size_t page_index;
  };

  Impl(bool direct_backend, std::size_t target_pages)
      : direct(direct_backend),
        // This value controls mapping growth, not the number of live leases.
        slab_pages(direct ? std::clamp(target_pages, MINIMUM_SLAB_PAGES, MAXIMUM_SLAB_PAGES) : 0) {}

  auto Reserve(std::size_t page_count) -> Allocation {
    const auto valid_request = direct && page_count != 0;
    TINYDB_CHECK(valid_request, "reserving pages from a heap arena");
    if (page_count > std::numeric_limits<std::size_t>::max() / PAGE_SIZE) {
      throw std::length_error("direct page-arena allocation is too large");
    }
    for (auto slab_index = std::size_t{0}; slab_index < slabs.size(); ++slab_index) {
      auto &slab = *slabs[slab_index];
      const auto found = std::ranges::find_if(slab.free_extents,
                                              [page_count](const auto &extent) { return extent.count >= page_count; });
      if (found == slab.free_extents.end()) {
        continue;
      }
      const auto page_index = found->offset;
      found->offset += page_count;
      found->count -= page_count;
      if (found->count == 0) {
        slab.free_extents.erase(found);
      }
      return {.slab_index = slab_index, .page_index = page_index};
    }

    // A large batch can exceed the normal slab size. One larger slab preserves
    // the contiguous-batch contract. Callers bound the requested page count.
    auto slab = std::make_unique<Slab>(std::max(slab_pages, page_count));
    const auto slab_index = slabs.size();
    const auto slab_count = slab->count;
    slab->free_extents.front().offset = page_count;
    slab->free_extents.front().count = slab_count - page_count;
    if (slab_count == page_count) {
      slab->free_extents.clear();
    }
    slabs.push_back(std::move(slab));
    return {.slab_index = slab_index, .page_index = 0};
  }

  const bool direct;
  const std::size_t slab_pages;
  std::mutex mutex;
  std::vector<std::unique_ptr<Slab>> slabs;
};

PageArena::PageArena(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PageArena::~PageArena() = default;

auto PageArena::CreateHeap() -> std::shared_ptr<PageArena> {
  return std::shared_ptr<PageArena>(new PageArena(std::make_unique<Impl>(false, 0)));
}

auto PageArena::CreateDirect(std::size_t target_pages) -> std::shared_ptr<PageArena> {
  return std::shared_ptr<PageArena>(new PageArena(std::make_unique<Impl>(true, target_pages)));
}

auto PageArena::Acquire() noexcept -> Lease {
  try {
    if (!impl_->direct) {
      return Lease(std::make_unique<PageBytes>());
    }
    auto owner = shared_from_this();
    auto lock = std::lock_guard(impl_->mutex);
    const auto allocation = impl_->Reserve(1);
    auto *const page = &impl_->slabs[allocation.slab_index]->pages[allocation.page_index];
    return {std::move(owner), allocation.slab_index, page};
  } catch (const std::bad_alloc &) {
    return {};
  } catch (const std::length_error &) {
    return {};
  } catch (...) {
    return {};
  }
}

auto PageArena::AcquireBatch(std::span<Lease> destination) noexcept -> bool {
  if (destination.empty()) {
    return true;
  }

  if (!impl_->direct) {
    try {
      // Hold every allocation here until the batch is complete, so failure
      // cannot replace only a prefix of destination.
      auto pages = std::vector<std::unique_ptr<PageBytes>>{};
      pages.reserve(destination.size());
      for (auto index = std::size_t{0}; index < destination.size(); ++index) {
        pages.push_back(std::make_unique<PageBytes>());
      }
      for (auto index = std::size_t{0}; index < destination.size(); ++index) {
        destination[index] = Lease(std::move(pages[index]));
      }
      return true;
    } catch (const std::bad_alloc &) {
      return false;
    } catch (const std::length_error &) {
      return false;
    }
  }

  try {
    // Build the leases separately so destination changes only after the arena
    // has reserved the complete contiguous extent.
    auto leases = std::vector<Lease>{};
    leases.reserve(destination.size());
    auto owner = shared_from_this();
    {
      auto lock = std::lock_guard(impl_->mutex);
      const auto allocation = impl_->Reserve(destination.size());
      auto *const pages = impl_->slabs[allocation.slab_index]->pages;
      for (auto index = std::size_t{0}; index < destination.size(); ++index) {
        leases.push_back(Lease(owner, allocation.slab_index, &pages[allocation.page_index + index]));
      }
    }
    for (auto index = std::size_t{0}; index < destination.size(); ++index) {
      destination[index] = std::move(leases[index]);
    }
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  } catch (const std::length_error &) {
    return false;
  } catch (...) {
    return false;
  }
}

void PageArena::Release(std::size_t slab_index, PageBytes *page) noexcept {
  auto lock = std::lock_guard(impl_->mutex);
  const auto known_slab = impl_->direct && slab_index < impl_->slabs.size();
  TINYDB_CHECK(known_slab, "returning a page to an unknown direct arena slab");
  auto &slab = *impl_->slabs[slab_index];
  const auto base = reinterpret_cast<std::uintptr_t>(slab.pages);
  const auto address = reinterpret_cast<std::uintptr_t>(page);
  const auto inside_slab = address >= base && address - base < PageBytesFor(slab.count);
  const auto at_page_boundary = address >= base && (address - base) % sizeof(PageBytes) == 0;
  const auto valid_page = inside_slab && at_page_boundary;
  TINYDB_CHECK(valid_page, "returning an invalid direct arena page");
  const auto page_index = (address - base) / sizeof(PageBytes);

  const auto insertion = std::ranges::lower_bound(slab.free_extents, page_index, {}, &Impl::FreeExtent::offset);
  const auto previous = insertion == slab.free_extents.begin() ? slab.free_extents.end() : std::prev(insertion);
  const auto follows_previous = previous == slab.free_extents.end() || previous->offset + previous->count <= page_index;
  const auto precedes_next = insertion == slab.free_extents.end() || page_index < insertion->offset;
  const auto valid_position = follows_previous && precedes_next;
  TINYDB_CHECK(valid_position, "direct arena page was returned twice");
  const auto joins_previous = previous != slab.free_extents.end() && previous->offset + previous->count == page_index;
  const auto joins_next = insertion != slab.free_extents.end() && page_index + 1U == insertion->offset;
  // Immediate neighbor merges keep the free list canonical. They also make the
  // largest possible contiguous extent available to the next batch.
  if (joins_previous && joins_next) {
    previous->count += 1U + insertion->count;
    slab.free_extents.erase(insertion);
  } else if (joins_previous) {
    ++previous->count;
  } else if (joins_next) {
    --insertion->offset;
    ++insertion->count;
  } else {
    slab.free_extents.insert(insertion, Impl::FreeExtent{.offset = page_index, .count = 1});
  }
}

void PageArena::ReleaseFreeMemory() noexcept {
  if (!impl_->direct) {
    return;
  }
  auto lock = std::lock_guard(impl_->mutex);
  for (const auto &slab : impl_->slabs) {
    for (const auto &extent : slab->free_extents) {
      auto *const pages = &slab->pages[extent.offset];
      const auto bytes = PageBytesFor(extent.count);
#if defined(MADV_REMOVE)
      // MADV_REMOVE releases RSS and shared-memory backing for free extents.
      // MADV_DONTNEED can retain that backing for shared anonymous mappings.
      if (::madvise(pages, bytes, MADV_REMOVE) == 0) {
        continue;
      }
#endif
      static_cast<void>(::madvise(pages, bytes, MADV_DONTNEED));
    }
  }
}

PageArena::Lease::Lease(std::unique_ptr<PageBytes> heap_page)
    : heap_page_(std::move(heap_page)), page_(heap_page_.get()) {}

PageArena::Lease::Lease(Lease &&other) noexcept
    : owner_(std::move(other.owner_)),
      heap_page_(std::move(other.heap_page_)),
      slab_index_(other.slab_index_),
      page_(std::exchange(other.page_, nullptr)) {}

auto PageArena::Lease::operator=(Lease &&other) noexcept -> Lease & {
  if (this != &other) {
    Reset();
    owner_ = std::move(other.owner_);
    heap_page_ = std::move(other.heap_page_);
    slab_index_ = other.slab_index_;
    page_ = std::exchange(other.page_, nullptr);
  }
  return *this;
}

PageArena::Lease::~Lease() { Reset(); }

auto PageArena::Lease::Bytes() noexcept -> PageBytes & {
  TINYDB_CHECK(page_ != nullptr, "accessing an empty page-arena lease");
  return *page_;
}

auto PageArena::Lease::Bytes() const noexcept -> const PageBytes & {
  TINYDB_CHECK(page_ != nullptr, "accessing an empty page-arena lease");
  return *page_;
}

void PageArena::Lease::Reset() noexcept {
  if (owner_ != nullptr && page_ != nullptr) {
    owner_->Release(slab_index_, page_);
  }
  page_ = nullptr;
  heap_page_.reset();
  owner_.reset();
}

}  // namespace tinydb::cache
