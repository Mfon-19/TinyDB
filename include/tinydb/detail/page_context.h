#pragma once

#include "tinydb/status.h"
#include "tinydb/storage/page_codec.h"
#include <atomic>
#include <cstdint>
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
  storage::PageMap pages{};
  std::uint64_t version = 0;
  Phase phase = Phase::Active;
};

class PageContext {
public:
  PageContext(cache::BufferPool &pool, const std::atomic<bool> &poisoned,
              WriteState *write = nullptr) noexcept
      : pool_(pool), poisoned_(poisoned), write_(write) {}

  [[nodiscard]] auto Active() const noexcept -> bool;
  [[nodiscard]] auto Version() const noexcept -> std::uint64_t;
  auto CheckActive() const -> Status;

  auto Fail(Status error) -> Status;
  [[nodiscard]] auto ReadPage(storage::PageId page_id)
      -> Result<storage::PageRef>;

  auto WritePage(const storage::Page &page) -> Status;
  [[nodiscard]] auto AllocatePage() -> Result<storage::PageId>;
  void FreePage(storage::PageId page_id);

private:
  cache::BufferPool &pool_;
  const std::atomic<bool> &poisoned_;
  WriteState *write_;
};

} // namespace tinydb::detail
