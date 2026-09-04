#pragma once

/*
 * This is the API to the database that callers use. Only one open Database
 * may own a file at a time, including within the same process.
 */

#include "tinydb/cache/buffer_pool.h"
#include "tinydb/read_transaction.h"
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
  static Result<std::unique_ptr<Database>>
  Open(std::string_view name, std::size_t buffer_pool_capacity);

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Database(Database &&) = delete;
  Database &operator=(Database &&) = delete;

  auto BeginRead() -> Result<std::unique_ptr<ReadTransaction>>;
  auto BeginWrite() -> Result<std::unique_ptr<WriteTransaction>>;

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  Status Put(std::string_view key, std::string_view value);
  auto Delete(std::string_view key) -> Result<bool>;

private:
  friend class ReadTransaction;
  friend class WriteTransaction;
  Database(storage::DiskManager disk_manager, std::size_t buffer_pool_capacity,
           storage::PageId root_page_id, storage::PageId page_count);

  cache::BufferPool buffer_pool_;
  storage::PageId root_page_id_;
  storage::PageId page_count_;
  std::vector<storage::PageId> free_pages_;
  // Lock order: writer -> visibility -> buffer pool.
  std::mutex writer_mutex_;
  std::shared_mutex visibility_mutex_;
  std::atomic<bool> poisoned_{false};
};
} // namespace tinydb
