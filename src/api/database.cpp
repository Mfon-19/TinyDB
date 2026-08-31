#include "tinydb/database.h"
#include <string_view>
#include <utility>

namespace tinydb {

Result<Database> Database::Open(std::string_view name) {
  auto disk_manager = storage::DiskManager::Open(name);

  if (!disk_manager) {
    return Err(std::move(disk_manager.error()));
  }

  return Database{std::move(*disk_manager)};
}

Status Database::WritePage(storage::PageId page_id,
                           const storage::PageBytes &page) {
  return disk_manager_.WritePage(page_id, page);
}

Status Database::ReadPage(storage::PageId page_id,
                          storage::PageBytes &page) const {
  return disk_manager_.ReadPage(page_id, page);
}
} // namespace tinydb
