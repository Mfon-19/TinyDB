#pragma once

/*
 * This is the API to the database that callers use.
 */

#include "tinydb/cache/buffer_pool.h"
#include <string_view>
#include <utility>

namespace tinydb {

class Database {
public:
  static Result<Database> Open(const std::string_view name);

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  Database(Database &&) noexcept = default;
  Database &operator=(Database &&) noexcept = default;

  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Status ReadPage(storage::PageId page_id, storage::PageBytes &page);

private:
  explicit Database(storage::DiskManager disk_manager)
      : buffer_pool_(std::move(disk_manager)) {}
  cache::BufferPool buffer_pool_;
};
} // namespace tinydb
