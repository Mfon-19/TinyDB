#pragma once

/*
 * This is the API to the database that callers use. Only one open Database
 * may own a file at a time, including within the same process.
 */

#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/cache/buffer_pool.h"
#include "tinydb/write_transaction.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
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

  // Only one writer may be active. Do not begin another write on the same
  // thread until the current transaction finishes. Concurrent database access
  // is not supported yet; visibility and buffer-pool locking are still pending.
  auto BeginWrite() -> Result<std::unique_ptr<WriteTransaction>>;

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  Status Put(std::string_view key, std::string_view value);
  auto Delete(std::string_view key) -> Result<bool>;
  // A cursor must not outlive this Database; writes invalidate existing
  // cursors.
  auto Seek(std::string_view key) -> Result<btree::Cursor>;

private:
  friend class WriteTransaction;
  Database(cache::BufferPool buffer_pool, storage::PageId root_page_id,
           storage::PageId page_count);

  cache::BufferPool buffer_pool_;
  storage::PageId root_page_id_;
  storage::PageId page_count_;
  std::vector<storage::PageId> free_pages_;
  std::mutex writer_mutex_;
  bool poisoned_ = false;
  detail::PageContext read_context_;
  btree::BPlusTree tree_;
};
} // namespace tinydb
