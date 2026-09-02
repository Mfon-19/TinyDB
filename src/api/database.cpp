#include "tinydb/database.h"
#include "tinydb/storage/superblock_codec.h"
#include <cassert>
#include <memory>
#include <string_view>
#include <utility>

namespace tinydb {

namespace {

auto Create(cache::BufferPool &buffer_pool) -> Result<storage::PageId> {
  auto superblock_page_id = buffer_pool.AllocatePage();
  if (!superblock_page_id) {
    return Err(std::move(superblock_page_id.error()));
  }
  assert(*superblock_page_id == storage::SUPERBLOCK_PAGE_ID);

  auto root_page_id = buffer_pool.AllocatePage();
  if (!root_page_id) {
    return Err(std::move(root_page_id.error()));
  }
  if (auto status = btree::BPlusTree{buffer_pool, *root_page_id}.Initialize();
      !status.Ok()) {
    return Err(std::move(status));
  }

  auto superblock = storage::EncodeSuperblock({*root_page_id});
  if (!superblock) {
    return Err(std::move(superblock.error()));
  }
  if (auto status =
          buffer_pool.WritePage(storage::SUPERBLOCK_PAGE_ID, *superblock);
      !status.Ok()) {
    return Err(std::move(status));
  }
  return *root_page_id;
}

auto Load(cache::BufferPool &buffer_pool) -> Result<storage::PageId> {
  auto page = buffer_pool.ReadPage(storage::SUPERBLOCK_PAGE_ID);
  if (!page) {
    return Err(std::move(page.error()));
  }
  auto superblock = storage::DecodeSuperblock(page->Bytes());
  if (!superblock) {
    return Err(std::move(superblock.error()));
  }
  return superblock->root_page_id;
}

} // namespace

Database::Database(cache::BufferPool buffer_pool, storage::PageId root_page_id)
    : buffer_pool_(std::move(buffer_pool)), tree_(buffer_pool_, root_page_id) {}

Result<std::unique_ptr<Database>>
Database::Open(std::string_view name, std::size_t buffer_pool_capacity) {
  auto disk_manager = storage::DiskManager::Open(name);
  if (!disk_manager) {
    return Err(std::move(disk_manager.error()));
  }

  const bool is_new = disk_manager->PageCount() == 0;
  cache::BufferPool buffer_pool(std::move(*disk_manager), buffer_pool_capacity);
  auto root_page_id = is_new ? Create(buffer_pool) : Load(buffer_pool);
  if (!root_page_id) {
    return Err(std::move(root_page_id.error()));
  }
  return std::unique_ptr<Database>(
      new Database(std::move(buffer_pool), *root_page_id));
}

auto Database::Get(std::string_view key) -> Result<std::optional<std::string>> {
  return tree_.Get(key);
}

Status Database::Put(std::string_view key, std::string_view value) {
  return tree_.Put(key, value);
}

auto Database::Delete(std::string_view key) -> Result<bool> {
  return tree_.Delete(key);
}

} // namespace tinydb
