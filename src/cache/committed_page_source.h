#pragma once

#include "btree/page_source.h"

namespace tinydb::cache {

class CommittedPageCache;

// Read-only bridge from immutable cache guards to the B+ tree's page-reader
// boundary. Write vocabulary is absent by type, not rejected at runtime.
class CommittedPageSource final : public PageReader {
 public:
  explicit CommittedPageSource(CommittedPageCache *cache) : cache_(cache) {}

  auto Read(page_id_t page_id) -> Result<PageHandle> override;

 private:
  CommittedPageCache *cache_;
};

}  // namespace tinydb::cache
