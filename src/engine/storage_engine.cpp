#include <tinydb/check.h>
#include <tinydb/storage_engine.h>
#include <tinydb/unique_fd.h>

#include "btree/b_plus_tree.h"
#include "cache/committed_page_cache.h"
#include "cache/committed_page_source.h"
#include "io/syscalls.h"
#include "txn/contract.h"
#include "txn/database_state.h"
#include "txn/read_snapshot.h"
#include "txn/reader_gate.h"
#include "txn/transaction_pages.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

constexpr std::size_t CACHE_TARGET_BYTES = 64 * PAGE_SIZE;
constexpr std::size_t WRITE_TRANSACTION_LIMIT_BYTES = 16U << 20U;
constexpr std::uint64_t CHECKPOINT_THRESHOLD_BYTES = 1U << 20U;

auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

auto SyncParentDirectory(const std::filesystem::path &path) -> Status {
  auto parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  auto directory = UniqueFd(io::Open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory.Valid()) {
    return ErrnoStatus("open directory");
  }
  if (io::Fsync(directory.Get()) < 0) {
    return ErrnoStatus("fsync directory");
  }
  return {};
}

// Ownership is acquired before recovery because replay and WAL truncation are
// writes. flock also rejects a second open file description in this process.
auto AcquireDatabaseLock(const std::filesystem::path &path) -> Result<UniqueFd> {
  auto fd = UniqueFd(io::Open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }
  if (io::Flock(fd.Get(), LOCK_EX | LOCK_NB) < 0) {
    if (errno == EWOULDBLOCK) {
      return std::unexpected(Status::Busy("database is locked by another handle: " + path.string()));
    }
    return std::unexpected(ErrnoStatus("flock"));
  }

  struct stat stat_buffer {};
  if (io::Fstat(fd.Get(), &stat_buffer) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  if (stat_buffer.st_size == 0) {
    if (auto status = SyncParentDirectory(path); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }
  return fd;
}

auto InitialState(const DiskManager &disk) -> std::shared_ptr<const txn::DatabaseState> {
  return std::make_shared<const txn::DatabaseState>(txn::DatabaseState{
      .root_page_id = disk.GetRootPageId(),
      .allocator_root_page_id = disk.GetAllocatorRootPageId(),
      .high_water_page_id = disk.HighWaterPageId(),
      .transaction_id = disk.TransactionId(),
      .visible_lsn = disk.TransactionId(),
      .checkpoint_lsn = disk.CheckpointLsn(),
  });
}

}  // namespace

auto StorageEngine::Open(const std::filesystem::path &path) -> Result<StorageEngine> {
  auto lock = AcquireDatabaseLock(path);
  if (!lock) {
    return std::unexpected(std::move(lock).error());
  }

  // Recovery precedes normal decoders and cache construction because replay
  // may replace either superblock and any data page.
  const auto wal_path = Wal::PathFor(path);
  if (auto status = Wal::Recover(path, wal_path); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto disk_result = DiskManager::Open(path);
  if (!disk_result) {
    return std::unexpected(std::move(disk_result).error());
  }
  auto disk = std::make_unique<DiskManager>(*std::move(disk_result));
  auto cache = std::make_unique<cache::CommittedPageCache>(disk.get(), CACHE_TARGET_BYTES, disk->CheckpointLsn());
  auto pages = std::make_unique<cache::CommittedPageSource>(cache.get());
  auto readers = std::make_unique<txn::ReaderGate>(InitialState(*disk));

  auto wal_result = Wal::Open(wal_path, disk->Uuid());
  if (!wal_result) {
    return std::unexpected(std::move(wal_result).error());
  }
  auto wal = std::make_unique<Wal>(*std::move(wal_result));

  auto engine = StorageEngine(path, std::move(*lock), std::move(disk), std::move(cache), std::move(pages),
                              std::move(readers), std::move(wal));
  // A fresh empty root is published through the same private-page and WAL
  // path as every later mutation; no unlogged bootstrap state is exposed.
  if (engine.disk_->GetRootPageId() == HEADER_PAGE_ID) {
    if (auto status = engine.Mutate(Mutation::Bootstrap); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }
  if (auto status = engine.CheckIntegrity(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return engine;
}

StorageEngine::StorageEngine(std::filesystem::path path, UniqueFd lock_fd, std::unique_ptr<DiskManager> disk,
                             std::unique_ptr<cache::CommittedPageCache> cache,
                             std::unique_ptr<cache::CommittedPageSource> pages,
                             std::unique_ptr<txn::ReaderGate> readers, std::unique_ptr<Wal> wal)
    : path_(std::move(path)),
      lock_fd_(std::move(lock_fd)),
      disk_(std::move(disk)),
      cache_(std::move(cache)),
      pages_(std::move(pages)),
      readers_(std::move(readers)),
      wal_(std::move(wal)),
      writer_mutex_(std::make_unique<std::mutex>()) {}

StorageEngine::StorageEngine(StorageEngine &&other) noexcept
    : path_(std::move(other.path_)),
      closed_(other.closed_),
      poisoned_(other.poisoned_),
      lock_fd_(std::move(other.lock_fd_)),
      disk_(std::move(other.disk_)),
      cache_(std::move(other.cache_)),
      pages_(std::move(other.pages_)),
      readers_(std::move(other.readers_)),
      wal_(std::move(other.wal_)),
      writer_mutex_(std::move(other.writer_mutex_)) {
  other.closed_ = true;
}

StorageEngine::~StorageEngine() { CloseBestEffort(); }

void StorageEngine::CloseBestEffort() noexcept {
  if (const auto status = Close(); !status.Ok()) {
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", path_.c_str(), status.ToString().c_str());
  }
}

auto StorageEngine::operator=(StorageEngine &&other) noexcept -> StorageEngine & {
  if (this == &other) {
    return *this;
  }
  CloseBestEffort();
  readers_.reset();
  pages_.reset();
  cache_.reset();
  disk_.reset();
  wal_.reset();

  path_ = std::move(other.path_);
  closed_ = other.closed_;
  poisoned_ = other.poisoned_;
  lock_fd_ = std::move(other.lock_fd_);
  disk_ = std::move(other.disk_);
  cache_ = std::move(other.cache_);
  pages_ = std::move(other.pages_);
  readers_ = std::move(other.readers_);
  wal_ = std::move(other.wal_);
  writer_mutex_ = std::move(other.writer_mutex_);
  other.closed_ = true;
  return *this;
}

auto StorageEngine::Put(std::string_view key, std::string_view value) -> Status {
  if (closed_) {
    return Status::Closed("Put on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("Put on a poisoned handle; reopen to recover");
  }
  if (key.size() + value.size() > MAX_ENTRY_BYTES) {
    return Status::InvalidArgument("key + value exceeds MAX_ENTRY_BYTES");
  }
  return Mutate(Mutation::Put, key, value);
}

auto StorageEngine::Get(std::string_view key) -> Result<std::optional<std::string>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Get on a closed handle"));
  }
  if (poisoned_) {
    return std::unexpected(Status::Closed("Get on a poisoned handle; reopen to recover"));
  }
  auto snapshot = txn::ReadSnapshot::Begin(readers_.get(), pages_.get());
  return snapshot.Get(key);
}

auto StorageEngine::Remove(std::string_view key) -> Status {
  if (closed_) {
    return Status::Closed("Remove on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("Remove on a poisoned handle; reopen to recover");
  }
  return Mutate(Mutation::Remove, key);
}

auto StorageEngine::Scan(std::string_view start,
                         std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Scan on a closed handle"));
  }
  if (poisoned_) {
    return std::unexpected(Status::Closed("Scan on a poisoned handle; reopen to recover"));
  }

  auto snapshot = txn::ReadSnapshot::Begin(readers_.get(), pages_.get());
  auto cursor = snapshot.Seek(start);
  if (!cursor) {
    return std::unexpected(std::move(cursor).error());
  }
  auto rows = std::vector<std::pair<std::string, std::string>>{};
  const auto less = txn::BytewiseLess{};
  while (cursor->Valid() && less(cursor->Key(), end)) {
    rows.emplace_back(cursor->Key(), cursor->Value());
    if (auto status = cursor->Next(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }
  return rows;
}

auto StorageEngine::CheckIntegrity() -> Status {
  if (closed_) {
    return Status::Closed("CheckIntegrity on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("CheckIntegrity on a poisoned handle; reopen to recover");
  }

  const auto state = readers_->CurrentState();
  auto transaction = txn::TransactionPages::Begin(pages_.get(), *state, WRITE_TRANSACTION_LIMIT_BYTES);
  if (!transaction) {
    return std::move(transaction).error();
  }
  auto free_pages = std::unordered_set<page_id_t>{};
  for (const auto &extent : transaction->FreeExtents()) {
    for (page_id_t page_id = extent.first_page_id; page_id < extent.first_page_id + extent.page_count; ++page_id) {
      free_pages.insert(page_id);
    }
  }
  const auto allocator_pages =
      std::unordered_set<page_id_t>(transaction->AllocatorPageIds().begin(), transaction->AllocatorPageIds().end());
  return BPlusTree::CheckIntegrity(pages_.get(), state->root_page_id, state->high_water_page_id, free_pages,
                                   allocator_pages);
}

auto StorageEngine::Mutate(Mutation mutation, std::string_view key, std::string_view value) -> Status {
  auto writer = std::lock_guard(*writer_mutex_);
  // Readers remain admitted during preparation and continue seeing base while
  // every B+ tree and allocator change lives in the private overlay.
  const auto base = readers_->CurrentState();
  auto transaction = txn::TransactionPages::Begin(pages_.get(), *base, WRITE_TRANSACTION_LIMIT_BYTES);
  if (!transaction) {
    return std::move(transaction).error();
  }

  auto root_page_id = base->root_page_id;
  if (root_page_id == HEADER_PAGE_ID) {
    auto root = transaction->Allocate();
    if (!root) {
      transaction->Abort();
      return std::move(root).error();
    }
    root_page_id = root->Id();
    root = PageHandle{};
  }
  auto tree = BPlusTree::Open(&*transaction, root_page_id);
  if (!tree) {
    transaction->Abort();
    return std::move(tree).error();
  }

  auto status = Status{};
  if (mutation == Mutation::Put) {
    status = transaction->ChargeValueBytes(value.size());
    if (status.Ok()) {
      status = tree->Put(key, value);
    }
  } else if (mutation == Mutation::Remove) {
    status = tree->Remove(key);
  }
  if (!status.Ok()) {
    transaction->Abort();
    return status;
  }
  transaction->SetRootPageId(tree->RootPageId());
  if (status = transaction->Freeze(base->visible_lsn + 1); !status.Ok()) {
    transaction->Abort();
    return status;
  }
  if (!transaction->HasChanges()) {
    return {};
  }

  // Everything through Freeze is a definite-abort region. No shared page,
  // root, frontier, or free extent has changed if one of those steps fails.
  const auto state = transaction->ResultingState();
  auto state_image = disk_->PrepareStateImage(state.root_page_id, state.allocator_root_page_id,
                                              state.high_water_page_id, state.transaction_id, state.checkpoint_lsn);
  if (!state_image) {
    return std::move(state_image).error();
  }
  auto retired = std::vector<page_id_t>(transaction->RetiredPageIds().begin(), transaction->RetiredPageIds().end());
  auto committed_pages = transaction->TakePages(state.transaction_id);
  if (!committed_pages) {
    return std::move(committed_pages).error();
  }
  auto published_state = std::make_shared<const txn::DatabaseState>(state);

  // The current WAL is a temporary durability bridge until Milestone 6. The
  // final ownership direction is already established: private images are
  // logged before they can become visible.
  for (const auto &page : *committed_pages) {
    wal_->AppendPageImage(page.page_id, page.bytes->data());
  }
  wal_->AppendPageImage(state_image->page_id, state_image->data.data());
  if (auto commit = wal_->Commit(); !commit.Ok()) {
    Poison();
    return commit;
  }

  {
    // Closing admission drains readers of base. Cache replacement, retirement,
    // and the new roots then become one visibility transition.
    auto publication = readers_->BeginPublication();
    for (auto &page : *committed_pages) {
      const auto installed = cache_->Install(std::move(page));
      TINYDB_CHECK(installed.Ok(), "durable page failed committed-cache installation");
    }
    cache_->Retire(retired);
    disk_->AdoptState(state.root_page_id, state.allocator_root_page_id, state.high_water_page_id, state.transaction_id,
                      state.checkpoint_lsn);
    publication.Publish(std::move(published_state));
  }

  if (wal_->SizeBytes() > CHECKPOINT_THRESHOLD_BYTES) {
    if (auto checkpoint = Checkpoint(); !checkpoint.Ok()) {
      Poison();
      return checkpoint;
    }
  }
  return {};
}

auto StorageEngine::Checkpoint() -> Status {
  const auto state = readers_->CurrentState();
  auto next = *state;
  next.checkpoint_lsn = state->visible_lsn;
  auto checkpointed_state = std::make_shared<const txn::DatabaseState>(std::move(next));
  // Materialize pages before either mirrored superblock advertises the new
  // checkpoint. The WAL remains authoritative until the final file sync.
  if (auto status = disk_->EnsurePageCount(state->high_water_page_id); !status.Ok()) {
    return status;
  }
  auto pages = cache_->DirtyPages();
  for (const auto &page : pages) {
    if (auto status = disk_->WritePage(page.Id(), page.Data().data()); !status.Ok()) {
      return status;
    }
  }
  disk_->AdvanceCheckpoint(state->visible_lsn);
  if (auto status = disk_->Checkpoint(); !status.Ok()) {
    return status;
  }
  if (auto status = disk_->Sync(); !status.Ok()) {
    return status;
  }
  if (auto status = wal_->Reset(); !status.Ok()) {
    return status;
  }
  cache_->MarkCheckpointed(state->visible_lsn);
  auto publication = readers_->BeginPublication();
  publication.Publish(std::move(checkpointed_state));
  return {};
}

void StorageEngine::Poison() {
  poisoned_ = true;
  wal_->DiscardPending();
}

auto StorageEngine::Close() -> Status {
  if (!disk_) {
    return {};
  }
  auto writer = std::lock_guard(*writer_mutex_);
  closed_ = true;

  if (!poisoned_) {
    if (auto status = Checkpoint(); !status.Ok()) {
      return status;
    }
  }
  readers_.reset();
  pages_.reset();
  cache_.reset();
  disk_.reset();
  wal_.reset();
  lock_fd_ = UniqueFd{};
  return {};
}

}  // namespace tinydb
