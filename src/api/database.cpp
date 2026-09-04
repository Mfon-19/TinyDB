#include "tinydb/database.h"
#include "tinydb/storage/superblock_codec.h"
#include <memory>
#include <string_view>
#include <utility>

namespace tinydb {

namespace {

auto Create(storage::DiskManager &disk_manager,
            std::string_view name) -> Result<storage::PageId> {
  constexpr storage::PageId root_page_id = 1;
  auto root =
      storage::EncodeLeafPage(root_page_id, storage::INVALID_PAGE_ID, {});
  if (!root) {
    return Err(std::move(root.error()));
  }
  if (auto status = disk_manager.WritePage(root_page_id, *root); !status.Ok()) {
    return Err(std::move(status));
  }

  auto superblock = storage::EncodeSuperblock({root_page_id});
  if (!superblock) {
    return Err(std::move(superblock.error()));
  }
  if (auto status =
          disk_manager.WritePage(storage::SUPERBLOCK_PAGE_ID, *superblock);
      !status.Ok()) {
    return Err(std::move(status));
  }
  if (auto status = disk_manager.Sync(); !status.Ok()) {
    return Err(std::move(status));
  }
  if (auto status = storage::SyncParentDirectory(name); !status.Ok()) {
    return Err(std::move(status));
  }
  return root_page_id;
}

auto Load(storage::DiskManager &disk_manager) -> Result<storage::PageId> {
  storage::PageBytes page{};
  if (auto status = disk_manager.ReadPage(storage::SUPERBLOCK_PAGE_ID, page);
      !status.Ok()) {
    return Err(std::move(status));
  }
  auto superblock = storage::DecodeSuperblock(page);
  if (!superblock) {
    return Err(std::move(superblock.error()));
  }
  return superblock->root_page_id;
}

} // namespace

Database::Database(storage::DiskManager disk_manager,
                   std::size_t buffer_pool_capacity,
                   storage::PageId root_page_id, storage::PageId page_count)
    : buffer_pool_(std::move(disk_manager), buffer_pool_capacity),
      root_page_id_(root_page_id), page_count_(page_count) {}

Result<std::unique_ptr<Database>>
Database::Open(std::string_view name, std::size_t buffer_pool_capacity) {
  auto disk_manager = storage::DiskManager::Open(name);
  if (!disk_manager) {
    return Err(std::move(disk_manager.error()));
  }

  const bool is_new = disk_manager->PageCount() == 0;
  auto root_page_id = is_new ? Create(*disk_manager, name) : Load(*disk_manager);
  if (!root_page_id) {
    return Err(std::move(root_page_id.error()));
  }
  const auto page_count = disk_manager->PageCount();
  auto database = std::unique_ptr<Database>(
      new Database(std::move(*disk_manager), buffer_pool_capacity,
                   *root_page_id, page_count));
  ReadTransaction transaction(*database);
  auto free_pages = transaction.tree_.FindFreePages(page_count);
  if (!free_pages) {
    return Err(std::move(free_pages.error()));
  }
  database->free_pages_ = std::move(*free_pages);
  return database;
}

auto Database::Get(std::string_view key) -> Result<std::optional<std::string>> {
  auto transaction = BeginRead();
  if (!transaction) {
    return Err(std::move(transaction.error()));
  }
  return (*transaction)->Get(key);
}

auto Database::BeginRead() -> Result<std::unique_ptr<ReadTransaction>> {
  auto transaction =
      std::unique_ptr<ReadTransaction>(new ReadTransaction(*this));
  if (auto status = transaction->context_.CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  return transaction;
}

auto Database::BeginWrite() -> Result<std::unique_ptr<WriteTransaction>> {
  auto transaction =
      std::unique_ptr<WriteTransaction>(new WriteTransaction(*this));
  if (auto status = transaction->context_.CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  return transaction;
}

Status Database::Put(std::string_view key, std::string_view value) {
  auto transaction = BeginWrite();
  if (!transaction) {
    return std::move(transaction.error());
  }
  if (auto status = (*transaction)->Put(key, value); !status.Ok()) {
    return status;
  }
  return (*transaction)->Commit();
}

auto Database::Delete(std::string_view key) -> Result<bool> {
  auto transaction = BeginWrite();
  if (!transaction) {
    return Err(std::move(transaction.error()));
  }
  auto removed = (*transaction)->Delete(key);
  if (!removed) {
    return Err(std::move(removed.error()));
  }
  if (auto status = (*transaction)->Commit(); !status.Ok()) {
    return Err(std::move(status));
  }
  return *removed;
}

} // namespace tinydb
