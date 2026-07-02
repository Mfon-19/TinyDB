#pragma once

#include <tinydb/buffer_pool.h>

namespace tinydb {

// Pins a page in the buffer pool for the lifetime of the object; unpins on
// destruction. Call MarkDirty() after writing through Data() so the unpin
// records the page as dirty.
class PageRef {
 public:
  PageRef(BufferPool *pool, page_id_t page_id) : pool_(pool), page_id_(page_id), data_(pool->FetchPage(page_id)) {}

  // Allocates a fresh (zeroed) page and returns it pinned.
  static auto New(BufferPool *pool) -> PageRef {
    page_id_t page_id = HEADER_PAGE_ID;
    char *data = pool->NewPage(&page_id);
    return {pool, page_id, data};
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
