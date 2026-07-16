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

// Compatibility facade for the current single-operation API. Reads use
// immutable committed snapshots; each mutation is prepared in a private page
// transaction before the existing WAL makes it durable. The explicit public
// transaction API and final commit coordinator arrive in Milestone 6.
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

  // The process lock is declared first and released last.
  UniqueFd lock_fd_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<cache::CommittedPageCache> cache_;
  std::unique_ptr<cache::CommittedPageSource> pages_;
  std::unique_ptr<txn::ReaderGate> readers_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<std::mutex> writer_mutex_;
};

}  // namespace tinydb
