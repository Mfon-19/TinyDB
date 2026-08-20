/*
 * This is the API to the database that callers use
 */

#include "tinydb/storage/disk_manager.h"
#include <algorithm>
#include <string_view>

namespace tinydb {

class Database {
public:
  static Result<Database> Open(const std::string_view name);

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  Database(Database &&) noexcept = default;
  Database &operator=(Database &&) noexcept = default;

  Status Write(const std::string_view buffer);
  Status Read(std::string &buffer);

private:
  explicit Database(storage::DiskManager disk_manager)
      : disk_manager_(std::move(disk_manager)) {}
  storage::DiskManager disk_manager_;
};
} // namespace tinydb