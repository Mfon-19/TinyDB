#pragma once

#include <tinydb/disk_manager.h>
#include <tinydb/limits.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>
#include <tinydb/wal.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

namespace cache {
class CommittedPageCache;
class CommittedPageSource;
}  // namespace cache
namespace txn {
class ReaderGate;
}  // namespace txn

/*
** Compatibility facade for the current single-operation API.
**
** Reads capture immutable committed snapshots. Each mutation acquires the
** sole writer permit, prepares B+ tree and allocator changes privately, makes
** their final physical images durable in WAL, drains readers of the old state,
** and publishes all new pages and roots together.
**
** Put and Remove are currently one-operation transactions. Milestone 6 exposes
** the same machinery as a multi-key WriteTransaction and replaces the
** temporary WAL/publication bridge with a prebuilt infallible commit plan.
*/
class StorageEngine {
 public:
  StorageEngine(const StorageEngine &) = delete;
  auto operator=(const StorageEngine &) -> StorageEngine & = delete;

  StorageEngine(StorageEngine &&other) noexcept;
  auto operator=(StorageEngine &&other) noexcept -> StorageEngine &;
  ~StorageEngine();

  static auto Open(const std::filesystem::path &path) -> Result<StorageEngine>;

  auto Put(std::string_view key, std::string_view value) -> Status;
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Remove(std::string_view key) -> Status;
  auto Scan(std::string_view start, std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>>;
  auto CheckIntegrity() -> Status;
  auto Close() -> Status;

 private:
  enum class Mutation { Bootstrap, Put, Remove };

  StorageEngine(std::filesystem::path path, UniqueFd lock_fd, std::unique_ptr<DiskManager> disk,
                std::unique_ptr<cache::CommittedPageCache> cache, std::unique_ptr<cache::CommittedPageSource> pages,
                std::unique_ptr<txn::ReaderGate> readers, std::unique_ptr<Wal> wal);

  void CloseBestEffort() noexcept;
  auto Mutate(Mutation mutation, std::string_view key = {}, std::string_view value = {}) -> Status;
  auto Checkpoint() -> Status;
  void Poison();

  std::filesystem::path path_;
  bool closed_{false};
  bool poisoned_{false};

  /*
  ** Members are declared in ownership order. The process lock is acquired
  ** before recovery and released last. Page sources borrow the cache, the
  ** cache borrows DiskManager, and snapshots borrow the page source during an
  ** admitted operation. Milestone 6 will expose those admissions publicly and
  ** make Close report Busy while one remains live.
  */
  UniqueFd lock_fd_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<cache::CommittedPageCache> cache_;
  std::unique_ptr<cache::CommittedPageSource> pages_;
  std::unique_ptr<txn::ReaderGate> readers_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<std::mutex> writer_mutex_;
};

}  // namespace tinydb
