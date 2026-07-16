#pragma once

#include <tinydb/buffer_pool.h>
#include <tinydb/status.h>

#include <expected>
#include <utility>

namespace tinydb {

// A scoped pin on one buffer-pool page: the pool's pin/unpin bookkeeping,
// done by object lifetime instead of by hand. While a PageRef lives, its
// page cannot be evicted and Data() stays valid; destruction returns the
// pin, reporting the page dirty iff MarkDirty() was called — so call it
// after every write through Data(), or the change may never reach disk.
class PageRef {
 public:
  // Pins page_id, fetching it from disk if it is not already cached.
  static auto Fetch(BufferPool *pool, page_id_t page_id) -> Result<PageRef> {
    auto data = pool->FetchPage(page_id);
    if (!data) {
      return std::unexpected(std::move(data).error());
    }
    return PageRef{pool, page_id, *data};
  }

  // Allocates a fresh (zeroed) page and returns it pinned.
  static auto New(BufferPool *pool) -> Result<PageRef> {
    auto page = pool->NewPage();
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    return PageRef{pool, page->page_id, page->data};
  }

  PageRef(const PageRef &) = delete;
  auto operator=(const PageRef &) -> PageRef & = delete;
  PageRef(PageRef &&other) noexcept
      : pool_(other.pool_), page_id_(other.page_id_), data_(other.data_), dirty_(other.dirty_) {
    other.pool_ = nullptr;
  }
  auto operator=(PageRef &&) -> PageRef & = delete;

  ~PageRef() {
    if (pool_ != nullptr) {
      pool_->UnpinPage(page_id_, dirty_);
    }
  }

  auto Data() -> char * { return data_; }
  auto Id() const -> page_id_t { return page_id_; }
  void MarkDirty() { dirty_ = true; }

 private:
  PageRef(BufferPool *pool, page_id_t page_id, char *data) : pool_(pool), page_id_(page_id), data_(data) {}

  BufferPool *pool_;
  page_id_t page_id_;
  char *data_;
  bool dirty_{false};
};

}  // namespace tinydb
