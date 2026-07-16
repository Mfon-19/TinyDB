#pragma once

#include "btree/page_source.h"

namespace tinydb {

class BufferPool;

// Adapts BufferPool pins to the tree's storage-neutral PageSource contract.
class BufferPoolPageSource final : public PageSource {
 public:
  explicit BufferPoolPageSource(BufferPool *pool) : pool_(pool) {}

  auto Read(page_id_t page_id) -> Result<PageHandle> override;
  auto Edit(page_id_t page_id) -> Result<PageHandle> override;
  auto Allocate() -> Result<PageHandle> override;
  auto Free(page_id_t page_id) -> Status override;

 private:
  static void ReleasePin(void *owner, page_id_t page_id, bool dirty);

  BufferPool *pool_;
};

}  // namespace tinydb
