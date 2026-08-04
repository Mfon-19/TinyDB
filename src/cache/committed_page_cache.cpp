#include "cache/committed_page_cache.h"

#include "storage/disk_manager.h"
#include "util/check.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "storage/page_codec.h"

#include <expected>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace tinydb::cache {

/*
** A frame owns exactly one immutable encoded page version. Its validated
** common header orders physical versions and travels with immutable handles,
** so consumers never checksum the same bytes twice. The header LSN and cache
** checkpoint LSN determine eviction eligibility, not visibility.
*/
struct CommittedFrame final {
  CommittedFrame(storage::DataPageHeader initial_header, std::unique_ptr<PageBytes> initial_bytes)
      : header(initial_header), bytes(std::move(initial_bytes)) {}

  storage::DataPageHeader header;    // checksum-authenticated common fields
  std::unique_ptr<PageBytes> bytes;  // immutable after construction

  /*
  ** CHECKPOINTED LRU LINKS
  **
  ** Every current checkpointed frame belongs to this intrusive list, including
  ** a temporarily pinned one. newer points toward the MRU end and older toward
  ** the LRU end. Eviction skips pins; release therefore only drops shared
  ** ownership and never reacquires the cache mutex.
  */
  CommittedFrame *newer{nullptr};
  CommittedFrame *older{nullptr};
  bool in_lru{false};
};

namespace {

auto IsTreePage(const storage::DataPageHeader &header) -> bool {
  return header.type == storage::DataPageType::Leaf || header.type == storage::DataPageType::Internal;
}

auto Lease(std::shared_ptr<CommittedFrame> frame) -> PageHandle {
  auto *const leased = frame.get();
  auto keeper = std::static_pointer_cast<const void>(std::move(frame));
  return {leased->header.page_id, leased->bytes->data(), std::move(keeper), &leased->header,
          IsTreePage(leased->header)};
}

}  // namespace

struct CommittedPageCache::Impl final {
  DiskManager &disk;  // backing checkpointed database file
  const std::size_t target_bytes;
  mutable std::mutex mutex;  // protects every field below
  std::vector<std::shared_ptr<CommittedFrame>> pages;
  std::size_t resident_pages{0};
  std::size_t loading_pages{0};
  std::size_t dirty_pages{0};
  CommittedFrame *most_recent{nullptr};
  CommittedFrame *least_recent{nullptr};
  std::uint64_t checkpoint_lsn;  // newest page LSN stored in the database file
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};

  Impl(DiskManager &database_file, std::size_t byte_target, std::uint64_t initial_checkpoint_lsn)
      : disk(database_file), target_bytes(byte_target), checkpoint_lsn(initial_checkpoint_lsn) {}

  auto IsCheckpointed(const CommittedFrame &frame) const -> bool { return frame.header.page_lsn <= checkpoint_lsn; }

  void LinkMostRecent(CommittedFrame *frame) {
    TINYDB_CHECK(!frame->in_lru, "linking a page already present in the LRU");
    TINYDB_CHECK(frame->newer == nullptr, "linking a page with a stale newer LRU link");
    TINYDB_CHECK(frame->older == nullptr, "linking a page with a stale older LRU link");
    TINYDB_CHECK(IsCheckpointed(*frame), "linking an uncheckpointed eviction candidate");

    frame->older = most_recent;
    if (most_recent != nullptr) {
      most_recent->newer = frame;
    } else {
      least_recent = frame;
    }
    most_recent = frame;
    frame->in_lru = true;
  }

  void Unlink(CommittedFrame *frame) {
    TINYDB_CHECK(frame->in_lru, "unlinking a page absent from the LRU");
    if (frame->newer != nullptr) {
      frame->newer->older = frame->older;
    } else {
      TINYDB_CHECK(most_recent == frame, "eviction MRU link is inconsistent");
      most_recent = frame->older;
    }
    if (frame->older != nullptr) {
      frame->older->newer = frame->newer;
    } else {
      TINYDB_CHECK(least_recent == frame, "eviction LRU link is inconsistent");
      least_recent = frame->newer;
    }
    frame->newer = nullptr;
    frame->older = nullptr;
    frame->in_lru = false;
  }

  void Touch(CommittedFrame *frame) {
    if (frame == most_recent) {
      return;
    }
    Unlink(frame);
    LinkMostRecent(frame);
  }

  void RecordAccess(const std::shared_ptr<CommittedFrame> &frame) {
    if (frame->in_lru) {
      Touch(frame.get());
    }
  }

  auto TakeEvictionCandidate() -> std::shared_ptr<CommittedFrame> {
    // Pinned frames remain linked so their final release needs no mutex. In
    // ordinary use they are near the MRU end, leaving victim selection O(1);
    // a long-lived reader may require a short walk past its pinned frames.
    auto *victim = least_recent;
    while (victim != nullptr) {
      const auto page_id = victim->header.page_id;
      TINYDB_CHECK(page_id < pages.size(), "LRU page lies outside the cache table");
      TINYDB_CHECK(pages[page_id].get() == victim, "LRU contains a stale frame");
      if (pages[page_id].use_count() == 1) {
        break;
      }
      victim = victim->newer;
    }
    if (victim == nullptr) {
      return {};
    }
    const auto victim_page_id = victim->header.page_id;
    Unlink(victim);
    auto frame = std::move(pages[victim_page_id]);
    --resident_pages;
    ++evictions;
    return frame;
  }

  void TrimToTarget() {
    while (resident_pages + loading_pages > target_bytes / PAGE_SIZE) {
      if (TakeEvictionCandidate() == nullptr) {
        break;
      }
    }
  }

  auto MakeRoomForRead() -> std::shared_ptr<CommittedFrame> {
    auto reusable = std::shared_ptr<CommittedFrame>{};
    while (resident_pages + loading_pages >= target_bytes / PAGE_SIZE) {
      auto frame = TakeEvictionCandidate();
      if (frame == nullptr) {
        break;
      }
      if (reusable == nullptr) {
        reusable = std::move(frame);
      }
    }
    return reusable;
  }

  auto HasRoomForRead() const -> bool { return resident_pages + loading_pages < target_bytes / PAGE_SIZE; }

  /*
  ** Every blocking and page-sized operation in a physical miss happens here,
  ** outside mutex. An unobserved eviction victim supplies its existing aligned
  ** buffer, avoiding allocator churn in steady-state scans and random reads.
  */
  auto LoadFrame(page_id_t page_id,
                 std::shared_ptr<CommittedFrame> frame) const -> Result<std::shared_ptr<CommittedFrame>> {
    try {
      auto new_bytes = std::unique_ptr<PageBytes>{};
      if (frame == nullptr) {
        new_bytes = std::make_unique<PageBytes>();
      }
      auto *const bytes = frame != nullptr ? frame->bytes.get() : new_bytes.get();
      if (auto status = disk.ReadPage(page_id, bytes->data()); !status.Ok()) {
        if (status.Code() == StatusCode::InvalidArgument) {
          return std::unexpected(Status::Corruption("tree references a page outside the allocated database"));
        }
        return std::unexpected(std::move(status));
      }
      const auto header =
          storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*bytes)), page_id);
      if (!header) {
        return std::unexpected(header.error());
      }
      const auto tree_page = IsTreePage(*header);
      if (tree_page) {
        if (auto status = ValidateTreePagePayload(bytes->data(), *header); !status.Ok()) {
          return std::unexpected(std::move(status));
        }
      }

      if (frame != nullptr) {
        frame->header = *header;
      } else {
        frame = std::make_shared<CommittedFrame>(*header, std::move(new_bytes));
      }
      return frame;
    } catch (const std::bad_alloc &) {
      return std::unexpected(Status::ResourceExhausted("committed page load allocation failed"));
    }
  }
};

CommittedPageCache::CommittedPageCache(DiskManager &disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn)
    : impl_(std::make_unique<Impl>(disk, target_bytes, checkpoint_lsn)) {}

CommittedPageCache::~CommittedPageCache() = default;

auto CommittedPageCache::Read(page_id_t page_id) -> Result<PageHandle> {
  auto reusable = std::shared_ptr<CommittedFrame>{};
  {
    auto lock = std::unique_lock(impl_->mutex);
    if (page_id < impl_->pages.size() && impl_->pages[page_id] != nullptr) {
      ++impl_->hits;
      impl_->RecordAccess(impl_->pages[page_id]);
      return Lease(impl_->pages[page_id]);
    }
    ++impl_->misses;

    // A loading page consumes the same hard capacity as a resident page.
    // Publication alone may temporarily exceed the target with dirty pages.
    reusable = impl_->MakeRoomForRead();
    if (!impl_->HasRoomForRead()) {
      return std::unexpected(
          Status::ResourceExhausted("committed cache is full of pinned, loading, or uncheckpointed pages"));
    }
    ++impl_->loading_pages;
  }

  auto loaded = impl_->LoadFrame(page_id, std::move(reusable));
  auto lock = std::unique_lock(impl_->mutex);
  --impl_->loading_pages;

  // A racing miss for the same page may have installed first. Reuse that
  // immutable version and discard this duplicate read instead of replacing it.
  if (page_id < impl_->pages.size() && impl_->pages[page_id] != nullptr) {
    impl_->RecordAccess(impl_->pages[page_id]);
    return Lease(impl_->pages[page_id]);
  }
  if (!loaded) {
    lock.unlock();
    return std::unexpected(std::move(loaded.error()));
  }
  if (page_id >= impl_->pages.size()) {
    try {
      impl_->pages.resize(page_id + 1);
    } catch (const std::bad_alloc &) {
      return std::unexpected(Status::ResourceExhausted("committed cache page table allocation failed"));
    } catch (const std::length_error &) {
      return std::unexpected(Status::ResourceExhausted("committed cache page table exceeds container limits"));
    }
  }
  auto &current = impl_->pages[page_id];
  current = std::move(*loaded);
  ++impl_->resident_pages;
  impl_->LinkMostRecent(current.get());
  return Lease(current);
}

auto CommittedPageCache::PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                                            page_id_t logical_page_count) -> Result<PublicationPlan> {
  if (logical_page_count < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("publication has an invalid logical page count"));
  }

  auto frames = std::vector<std::shared_ptr<CommittedFrame>>{};
  frames.reserve(images.size());

  /*
  ** PREPARE CACHE OWNERSHIP
  **
  ** Grow the dense table and allocate every shared control block here. The
  ** writer has not appended WAL yet, so any validation or allocation failure
  ** remains a definite abort.
  */
  auto lock = std::lock_guard(impl_->mutex);
  if (impl_->pages.size() < logical_page_count) {
    impl_->pages.resize(logical_page_count);
  }
  auto previous_page_id = HEADER_PAGE_ID;
  for (auto &image : images) {
    const auto &header = image.header;
    if (image.bytes == nullptr || header.page_id < FIRST_DATA_PAGE_ID || header.page_id >= logical_page_count ||
        header.page_id <= previous_page_id || header.page_lsn == 0 ||
        header.payload_bytes > PAGE_SIZE - storage::data_page_offset::HEADER_BYTES) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid or duplicate page image"));
    }
    previous_page_id = header.page_id;
    const auto tree_page = IsTreePage(header);
    const auto non_tree_page =
        header.type == storage::DataPageType::Allocator || header.type == storage::DataPageType::Overflow;
    if (!tree_page && !non_tree_page) {
      return std::unexpected(Status::InvalidArgument("publication contains an unknown page type"));
    }
    if (tree_page && !image.tree_payload_validated) {
      if (auto status = ValidateTreePagePayload(image.bytes->data(), header); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
    }
    if (!tree_page && image.tree_payload_validated) {
      return std::unexpected(Status::InvalidArgument("non-tree publication carries a tree validation proof"));
    }
    const auto &existing = impl_->pages[header.page_id];
    if (existing != nullptr && existing->header.page_lsn > header.page_lsn) {
      return std::unexpected(Status::InvalidArgument("committed page version moves backward"));
    }
    frames.push_back(std::make_shared<CommittedFrame>(header, std::move(image.bytes)));
  }
  previous_page_id = HEADER_PAGE_ID;
  auto image_index = std::size_t{0};
  for (const auto page_id : retired) {
    while (image_index < images.size() && images[image_index].header.page_id < page_id) {
      ++image_index;
    }
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count || page_id <= previous_page_id ||
        (image_index < images.size() && images[image_index].header.page_id == page_id)) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid retired page"));
    }
    previous_page_id = page_id;
  }
  return PublicationPlan(std::move(frames), std::move(retired));
}

void CommittedPageCache::Publish(PublicationPlan plan) noexcept {
  auto lock = std::lock_guard(impl_->mutex);

  for (const auto page_id : plan.retired_) {
    if (impl_->pages[page_id] == nullptr) {
      continue;
    }
    if (impl_->pages[page_id]->in_lru) {
      impl_->Unlink(impl_->pages[page_id].get());
    }
    impl_->dirty_pages -= !impl_->IsCheckpointed(*impl_->pages[page_id]) ? 1U : 0U;
    impl_->pages[page_id].reset();
    --impl_->resident_pages;
  }
  for (auto &frame : plan.frames_) {
    auto &current = impl_->pages[frame->header.page_id];
    if (current == nullptr) {
      ++impl_->resident_pages;
    } else {
      impl_->dirty_pages -= !impl_->IsCheckpointed(*current) ? 1U : 0U;
      if (current->in_lru) {
        impl_->Unlink(current.get());
      }
    }
    current = std::move(frame);
    impl_->dirty_pages += !impl_->IsCheckpointed(*current) ? 1U : 0U;
    if (impl_->IsCheckpointed(*current)) {
      impl_->LinkMostRecent(current.get());
    }
  }
  impl_->TrimToTarget();
}

void CommittedPageCache::MarkCheckpointed(std::uint64_t checkpoint_lsn) {
  auto lock = std::lock_guard(impl_->mutex);
  const auto previous_checkpoint_lsn = impl_->checkpoint_lsn;
  impl_->checkpoint_lsn = checkpoint_lsn;

  // The database file now contains versions at or before checkpoint_lsn.
  // Later committed versions remain WAL-backed and non-evictable.
  for (const auto &page : impl_->pages) {
    if (page != nullptr && page->header.page_lsn > previous_checkpoint_lsn && page->header.page_lsn <= checkpoint_lsn) {
      --impl_->dirty_pages;
      if (!page->in_lru) {
        impl_->LinkMostRecent(page.get());
      }
    }
  }
  impl_->TrimToTarget();
}

auto CommittedPageCache::CaptureDirtyPages() -> std::vector<PageHandle> {
  auto lock = std::lock_guard(impl_->mutex);
  // Return guards in page-ID order. A later publication may replace a table
  // entry, but the guard keeps this exact old immutable version alive until
  // checkpoint I/O finishes.
  auto result = std::vector<PageHandle>{};
  result.reserve(impl_->dirty_pages);
  for (const auto &page : impl_->pages) {
    if (page != nullptr && !impl_->IsCheckpointed(*page)) {
      result.push_back(Lease(page));
    }
  }
  return result;
}

auto CommittedPageCache::DirtyPages() const -> std::size_t {
  auto lock = std::lock_guard(impl_->mutex);
  return impl_->dirty_pages;
}

auto CommittedPageCache::Stats() const -> CommittedCacheStats {
  auto lock = std::lock_guard(impl_->mutex);
  auto pinned_pages = std::size_t{0};
  for (const auto &page : impl_->pages) {
    if (page == nullptr) {
      continue;
    }
    pinned_pages += page.use_count() > 1 ? 1U : 0U;
  }
  return CommittedCacheStats{
      .target_bytes = impl_->target_bytes,
      .resident_bytes = impl_->resident_pages * PAGE_SIZE,
      .resident_pages = impl_->resident_pages,
      .pinned_pages = pinned_pages,
      .dirty_pages = impl_->dirty_pages,
      .hits = impl_->hits,
      .misses = impl_->misses,
      .evictions = impl_->evictions,
  };
}

}  // namespace tinydb::cache
