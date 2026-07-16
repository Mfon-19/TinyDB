#pragma once

#include "btree/page_source.h"

namespace tinydb::cache {

class CommittedPageCache;

/*
** Read-only bridge from cache guards to the tree's PageReader boundary.
** Write vocabulary is absent by type rather than rejected at runtime. Guard
** conversion transfers the existing cache pin and shared frame lifetime into
** a generic PageHandle without copying encoded bytes.
*/
class CommittedPageSource final : public PageReader {
 public:
  explicit CommittedPageSource(CommittedPageCache *cache) : cache_(cache) {}

  auto Read(page_id_t page_id) -> Result<PageHandle> override;

 private:
  CommittedPageCache *cache_;
};

}  // namespace tinydb::cache
