#pragma once

#include "tinydb/status.h"
#include "tinydb/storage/page_codec.h"
#include <atomic>
#include <map>
#include <vector>

namespace tinydb::cache {
class BufferPool;
}

namespace tinydb::detail {

inline constexpr auto POISONED_DATABASE_MESSAGE =
    "database is poisoned; close and reopen";

struct WriteState {
  enum class Phase { Active, Failed, Finished };

  storage::PageId page_count;
  std::vector<storage::PageId> free_pages;
  std::map<storage::PageId, storage::PageBytes> pages{};
  Phase phase = Phase::Active;
};

class PageContext {
public:
  PageContext(cache::BufferPool &pool, const std::atomic<bool> &poisoned,
              WriteState *write = nullptr) noexcept
      : pool_(pool), poisoned_(poisoned), write_(write) {}

  [[nodiscard]] bool Active() const noexcept;
  [[nodiscard]] bool ReadOnly() const noexcept { return write_ == nullptr; }
  Status CheckActive() const;
  Result<storage::Page> ReadPage(storage::PageId page_id);
  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Result<storage::PageId> AllocatePage();
  void FreePage(storage::PageId page_id);

private:
  cache::BufferPool &pool_;
  const std::atomic<bool> &poisoned_;
  WriteState *write_;
};

} // namespace tinydb::detail
