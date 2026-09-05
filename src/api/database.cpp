#include "tinydb/database.h"
#include "tinydb/storage/superblock_codec.h"
#include <cassert>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

namespace tinydb {

namespace {

Status Create(storage::DiskManager &disk_manager) {
  constexpr storage::PageId root_page_id = 1;
  auto root =
      storage::EncodeLeafPage(root_page_id, storage::INVALID_PAGE_ID, {});
  if (!root) {
    return std::move(root.error());
  }
  if (auto status = disk_manager.WritePage(root_page_id, *root); !status.Ok()) {
    return status;
  }
  auto superblock = storage::EncodeSuperblock({root_page_id});
  if (!superblock) {
    return std::move(superblock.error());
  }
  if (auto status =
          disk_manager.WritePage(storage::SUPERBLOCK_PAGE_ID, *superblock);
      !status.Ok()) {
    return status;
  }
  return disk_manager.Sync();
}

Status Recover(storage::DiskManager &disk_manager, storage::Wal &wal) {
  auto pages = wal.Validate();
  if (!pages) {
    return std::move(pages.error());
  }
  for (const auto &[page_id, page] : *pages) {
    if (auto status = disk_manager.WritePage(page_id, page); !status.Ok()) {
      return status;
    }
  }
  if (auto status = disk_manager.Sync(); !status.Ok()) {
    return status;
  }
  return wal.Reset();
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

Database::Database(storage::DiskManager disk_manager, storage::Wal wal,
                   std::size_t buffer_pool_capacity,
                   storage::PageId root_page_id, storage::PageId page_count)
    : buffer_pool_(std::move(disk_manager), buffer_pool_capacity),
      wal_(std::move(wal)), root_page_id_(root_page_id),
      page_count_(page_count) {}

Result<std::unique_ptr<Database>>
Database::Open(std::string_view name, std::size_t buffer_pool_capacity) {
  if (buffer_pool_capacity == 0) {
    return Err(
        Status::InvalidArgument("buffer pool capacity must be positive"));
  }
  auto disk_manager = storage::DiskManager::Open(name);
  if (!disk_manager) {
    return Err(std::move(disk_manager.error()));
  }

  std::error_code error;
  const auto path = std::filesystem::canonical(name, error).string();
  if (error) {
    return Err(Status::IoError(
        std::format("failed to resolve database path: {}", error.message())));
  }
  auto wal = storage::Wal::Open(path);
  if (!wal) {
    return Err(std::move(wal.error()));
  }
  if (disk_manager->PageCount() == 0) {
    if (!wal->Empty()) {
      return Err(Status::Corruption("nonempty WAL beside an empty database"));
    }
    if (auto status = Create(*disk_manager); !status.Ok()) {
      return Err(std::move(status));
    }
    if (auto status = wal->Sync(); !status.Ok()) {
      return Err(std::move(status));
    }
  } else if (auto status = Recover(*disk_manager, *wal); !status.Ok()) {
    return Err(std::move(status));
  }
  if (auto status = storage::SyncParentDirectory(path); !status.Ok()) {
    return Err(std::move(status));
  }
  auto root_page_id = Load(*disk_manager);
  if (!root_page_id) {
    return Err(std::move(root_page_id.error()));
  }
  const auto page_count = disk_manager->PageCount();
  auto database = std::unique_ptr<Database>(
      new Database(std::move(*disk_manager), std::move(*wal),
                   buffer_pool_capacity, *root_page_id, page_count));
  detail::PageContext context(database->buffer_pool_, database->poisoned_);
  btree::BPlusTree tree(context, *root_page_id);
  auto free_pages = tree.FindFreePages(page_count);
  if (!free_pages) {
    return Err(std::move(free_pages.error()));
  }
  database->free_pages_ = std::move(*free_pages);
  return database;
}

auto Database::Get(std::string_view key) -> Result<std::optional<std::string>> {
  ReadTransaction transaction(*this);
  return transaction.Get(key);
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
  WriteTransaction transaction(*this);
  if (auto status = transaction.Put(key, value); !status.Ok()) {
    return status;
  }
  return transaction.Commit();
}

auto Database::Delete(std::string_view key) -> Result<bool> {
  WriteTransaction transaction(*this);
  auto removed = transaction.Delete(key);
  if (!removed) {
    return Err(std::move(removed.error()));
  }
  if (auto status = transaction.Commit(); !status.Ok()) {
    return Err(std::move(status));
  }
  return *removed;
}

Status Database::Poison(std::string_view failure, const Status &status) {
  poisoned_ = true;
  return Status::IoError(
      std::format("{}: {}; close and reopen", failure, status.Message()));
}

Status Database::Commit(detail::WriteState &pending) {
  if (pending.pages.empty()) {
    return {};
  }
  auto record = storage::EncodeWalRecord(pending.pages);
  if (!record) {
    return std::move(record.error());
  }
  if (auto status = wal_.Append(*record); !status.Ok()) {
    return Poison("commit outcome is unknown", status);
  }
  if (auto status = wal_.Sync(); !status.Ok()) {
    return Poison("commit outcome is unknown", status);
  }

  std::unique_lock visibility(visibility_mutex_);
  if (auto status = Publish(pending); !status.Ok()) {
    return Poison("transaction is durable but publication failed", status);
  }
  return {};
}

Status Database::Publish(detail::WriteState &pending) {
  const auto threshold = buffer_pool_.Capacity() / 2;
  assert(wal_frames_ <= threshold);
  if (pending.pages.size() > threshold - wal_frames_) {
    if (auto status = CheckpointLocked(pending.pages); !status.Ok()) {
      return status;
    }
  } else {
    for (const auto &[page_id, page] : pending.pages) {
      if (auto status = buffer_pool_.InstallPage(page_id, page); !status.Ok()) {
        return status;
      }
    }
    wal_frames_ += pending.pages.size();
  }
  page_count_ = pending.page_count;
  free_pages_ = std::move(pending.free_pages);
  return {};
}

Status Database::CheckpointLocked(const storage::WalPages &incoming) {
  if (auto status = buffer_pool_.Flush(incoming); !status.Ok()) {
    return status;
  }
  if (auto status = wal_.Reset(); !status.Ok()) {
    return status;
  }
  wal_frames_ = 0;
  return {};
}

Status Database::Checkpoint() {
  std::unique_lock writer(writer_mutex_);
  if (poisoned_) {
    return Status::IoError(detail::POISONED_DATABASE_MESSAGE);
  }
  std::unique_lock visibility(visibility_mutex_);
  if (auto status = CheckpointLocked({}); !status.Ok()) {
    return Poison("checkpoint failed", status);
  }
  return {};
}

} // namespace tinydb
