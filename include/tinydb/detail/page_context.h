#pragma once

#include "tinydb/status.h"
#include "tinydb/storage/page.h"
#include <map>
#include <vector>

namespace tinydb::cache {
class BufferPool;
}

namespace tinydb::detail {

struct WriteState {
  enum class Phase { Active, Failed, Finished };

  storage::PageId page_count;
  std::vector<storage::PageId> free_pages;
  std::map<storage::PageId, storage::PageBytes> pages{};
  Phase phase = Phase::Active;
};

// The tree owns its read copies. Replacing a private image cannot invalidate
// page views held by a recursive tree operation, and pool pins stay temporary.
class PageContext {
public:
  PageContext(cache::BufferPool &pool, const bool &poisoned,
              WriteState *write = nullptr) noexcept
      : pool_(pool), poisoned_(poisoned), write_(write) {}

  [[nodiscard]] bool Active() const noexcept;
  Status CheckActive() const;
  Result<storage::PageBytes> ReadPage(storage::PageId page_id);
  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Result<storage::PageId> AllocatePage();
  void FreePage(storage::PageId page_id);

private:
  cache::BufferPool &pool_;
  const bool &poisoned_;
  WriteState *write_;
};

} // namespace tinydb::detail
