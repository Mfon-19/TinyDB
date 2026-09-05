#pragma once

/*
 * This is the API to the database that callers use. Only one open Database
 * may own a file at a time, including within the same process.
 */

#include "tinydb/cache/buffer_pool.h"
#include "tinydb/read_transaction.h"
#include "tinydb/storage/wal.h"
#include "tinydb/write_transaction.h"
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb {

class Database {
public:
  [[nodiscard]] static auto Open(std::string_view name,
                                 std::size_t buffer_pool_capacity)
      -> Result<std::unique_ptr<Database>>;

  Database(const Database &) = delete;
  auto operator=(const Database &) -> Database & = delete;
  Database(Database &&) = delete;
  auto operator=(Database &&) -> Database & = delete;

  [[nodiscard]] auto BeginRead() -> Result<std::unique_ptr<ReadTransaction>>;
  [[nodiscard]] auto BeginWrite() -> Result<std::unique_ptr<WriteTransaction>>;
  [[nodiscard]] auto Get(std::string_view key)
      -> Result<std::optional<std::string>>;
  auto Put(std::string_view key, std::string_view value) -> Status;
  [[nodiscard]] auto Delete(std::string_view key) -> Result<bool>;
  auto Checkpoint() -> Status;

private:
  friend class ReadTransaction;
  friend class WriteTransaction;
  Database(storage::DiskManager disk_manager, storage::Wal wal,
           std::size_t buffer_pool_capacity, storage::PageId root_page_id,
           storage::PageId page_count);
  auto Commit(detail::WriteState &pending) -> Status;
  auto Publish(detail::WriteState &pending) -> Status;
  auto CheckpointLocked(const storage::PageMap &incoming) -> Status;
  auto Poison(std::string_view failure, const Status &status) -> Status;

  cache::BufferPool buffer_pool_;
  storage::Wal wal_;
  const storage::PageId root_page_id_;
  storage::PageId page_count_;
  std::vector<storage::PageId> free_pages_;
  std::size_t wal_frames_ = 0;
  // Lock order: writer -> visibility -> buffer pool.
  std::mutex writer_mutex_;
  std::shared_mutex visibility_mutex_;
  std::atomic<bool> poisoned_{false};
};
} // namespace tinydb
