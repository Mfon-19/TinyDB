#include "btree/buffer_pool_page_source.h"

#include <tinydb/buffer_pool.h>
#include <tinydb/check.h>

#include <expected>
#include <utility>

namespace tinydb {

auto BufferPoolPageSource::Read(page_id_t page_id) -> Result<PageHandle> {
  // FetchPage pins on success; PageHandle's callback supplies the matching unpin.
  auto page = pool_->FetchPage(page_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }
  return PageHandle(pool_, page_id, *page, /*editable=*/false, ReleasePin);
}

auto BufferPoolPageSource::Edit(page_id_t page_id) -> Result<PageHandle> {
  // Read and Edit pin identically. editable_ controls whether tree code may
  // obtain char* and propagate a dirty bit at release.
  auto page = pool_->FetchPage(page_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }
  return PageHandle(pool_, page_id, *page, /*editable=*/true, ReleasePin);
}

auto BufferPoolPageSource::Allocate() -> Result<PageHandle> {
  // NewPage already returns a pinned, zero-filled frame.
  auto page = pool_->NewPage();
  if (!page) {
    return std::unexpected(std::move(page).error());
  }
  return PageHandle(pool_, page->page_id, page->data, /*editable=*/true, ReleasePin);
}

auto BufferPoolPageSource::Free(page_id_t page_id) -> Status {
  // BufferPool retirement is currently infallible; PageSource retains Status
  // because other owners can fail while recording retirement.
  pool_->FreePage(page_id);
  return {};
}

void BufferPoolPageSource::ReleasePin(void *owner, page_id_t page_id, bool dirty) {
  TINYDB_CHECK(owner != nullptr, "page handle has no buffer pool owner");
  static_cast<BufferPool *>(owner)->UnpinPage(page_id, dirty);
}

}  // namespace tinydb
