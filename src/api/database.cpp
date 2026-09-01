#include "tinydb/database.h"
#include <string_view>
#include <utility>

namespace tinydb {

Result<Database> Database::Open(std::string_view name,
                                std::size_t buffer_pool_capacity) {
  auto disk_manager = storage::DiskManager::Open(name);

  if (!disk_manager) {
    return Err(std::move(disk_manager.error()));
  }

  return Database{std::move(*disk_manager), buffer_pool_capacity};
}

Status Database::WritePage(storage::PageId page_id,
                           const storage::PageBytes &page) {
  return buffer_pool_.WritePage(page_id, page);
}

Result<cache::PageHandle> Database::ReadPage(storage::PageId page_id) {
  return buffer_pool_.ReadPage(page_id);
}
} // namespace tinydb
