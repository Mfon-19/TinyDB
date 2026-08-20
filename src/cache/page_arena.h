#pragma once

#include "storage/page.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace tinydb::cache {

using PageBytes = std::array<char, PAGE_SIZE>;
static_assert(sizeof(PageBytes) == PAGE_SIZE);
static_assert(alignof(PageBytes) == alignof(char));

/*
** PAGE OWNERSHIP ARENA
**
** A Lease owns one mutable 4 KiB page. A transaction lease moves unchanged
** into an immutable committed frame. Buffered caches obtain pages from
** ordinary heap allocations. Direct caches obtain pages from mmap-backed
** slabs whose runtime addresses satisfy O_DIRECT, so PageBytes itself keeps
** natural alignment and buffered allocations remain compact.
**
** Lease is the common ownership currency for copy-on-write, cache publication,
** read-ahead staging, and checkpoint I/O. Direct mode can retain one aligned
** allocation across transaction publication and checkpoint I/O, and request
** ownership prevents reclamation while the kernel references that buffer.
** Buffered mode avoids the allocator and RSS cost of making every PageBytes
** type globally over-aligned.
**
** A direct arena adds a slab only when no free extent can satisfy a request.
** AcquireBatch reserves one contiguous extent for direct I/O, while the heap
** arena returns independent allocations. In either case failure leaves the
** caller's destination unchanged. Slab size controls only the mapping growth
** increment, not the number of live leases.
*/
class PageArena final : public std::enable_shared_from_this<PageArena> {
 public:
  class Lease final {
   public:
    Lease() = default;
    Lease(const Lease &) = delete;
    auto operator=(const Lease &) -> Lease & = delete;
    Lease(Lease &&other) noexcept;
    auto operator=(Lease &&other) noexcept -> Lease &;
    ~Lease();

    explicit operator bool() const noexcept { return page_ != nullptr; }
    auto operator*() noexcept -> PageBytes & { return Bytes(); }
    auto operator*() const noexcept -> const PageBytes & { return Bytes(); }
    auto operator->() noexcept -> PageBytes * { return &Bytes(); }
    auto operator->() const noexcept -> const PageBytes * { return &Bytes(); }
    auto Bytes() noexcept -> PageBytes &;
    auto Bytes() const noexcept -> const PageBytes &;

   private:
    explicit Lease(std::unique_ptr<PageBytes> heap_page);
    Lease(std::shared_ptr<PageArena> owner, std::size_t slab_index, PageBytes *page)
        : owner_(std::move(owner)), slab_index_(slab_index), page_(page) {}
    void Reset() noexcept;

    std::shared_ptr<PageArena> owner_;
    std::unique_ptr<PageBytes> heap_page_;
    std::size_t slab_index_{0};
    PageBytes *page_{nullptr};

    friend class PageArena;
  };

  static auto CreateHeap() -> std::shared_ptr<PageArena>;
  static auto CreateDirect(std::size_t target_pages) -> std::shared_ptr<PageArena>;

  PageArena(const PageArena &) = delete;
  auto operator=(const PageArena &) -> PageArena & = delete;
  ~PageArena();

  auto Acquire() noexcept -> Lease;

  auto AcquireBatch(std::span<Lease> destination) noexcept -> bool;

  // A direct arena retains virtual mappings but asks the kernel to discard
  // physical storage for free extents. Heap arenas have nothing to release.
  void ReleaseFreeMemory() noexcept;

 private:
  struct Impl;
  explicit PageArena(std::unique_ptr<Impl> impl);
  void Release(std::size_t slab_index, PageBytes *page) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace tinydb::cache
