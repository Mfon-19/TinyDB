#pragma once

/*
 * This is the API to the database that callers use.
 */

#include "tinydb/storage/disk_manager.h"
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
  Status ReadPage(storage::PageId page_id, storage::PageBytes &page) const;

private:
  explicit Database(storage::DiskManager disk_manager)
      : disk_manager_(std::move(disk_manager)) {}
  storage::DiskManager disk_manager_;
};
} // namespace tinydb
