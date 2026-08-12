#pragma once

#include "storage/page.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace tinydb::cache {

/*
** PageBytes keeps normal byte alignment. Buffered mode therefore uses compact,
** independent heap allocations. Direct arenas get runtime O_DIRECT alignment
** from mmap, so this common page type needs no larger static alignment.
*/
using PageBytes = std::array<char, PAGE_SIZE>;
static_assert(sizeof(PageBytes) == PAGE_SIZE);
static_assert(alignof(PageBytes) == alignof(char));

/*
** PAGE OWNERSHIP ARENA
**
** A Lease owns one mutable 4 KiB page. A heap lease owns its allocation. A
** direct lease keeps its PageArena alive and identifies one page in a slab.
**
** When no free extent fits a request, a direct arena adds a slab. The cache,
** transaction, and staging limits bound live pages. The slab size controls
** only the growth increment.
**
** Commit moves a lease from a write transaction into a committed frame. This
** ownership transfer does not copy the page.
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

  // Direct arenas return one contiguous run. Heap arenas return independent
  // pages. A failure leaves the destination unchanged in both modes.
  auto AcquireBatch(std::span<Lease> destination) noexcept -> bool;

  // Direct arenas keep their mappings and ask the kernel to release free
  // physical pages. Active leases stay mapped and unchanged. Heap leases
  // release their allocations immediately, so heap arenas do no work here.
  void ReleaseFreeMemory() noexcept;

 private:
  struct Impl;
  explicit PageArena(std::unique_ptr<Impl> impl);
  void Release(std::size_t slab_index, PageBytes *page) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace tinydb::cache
