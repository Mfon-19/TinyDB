#include "tinydb/database.h"
#include <string>
#include <string_view>

namespace tinydb {

Result<Database> Database::Open(std::string_view name) {
  auto disk_manager = storage::DiskManager::Open(name);

  if (!disk_manager.has_value()) {
    return Err(Status::IoError("disk manager failed"));
  }

  return Database{std::move(*disk_manager)};
}

Status Database::Write(const std::string_view buffer) {
  return disk_manager_.Write(buffer);
}

Status Database::Read(std::string &buffer) {
  return disk_manager_.Read(buffer);
}
} // namespace tinydb