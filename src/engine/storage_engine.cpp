#include <tinydb/check.h>
#include <tinydb/disk_manager.h>
#include <tinydb/limits.h>
#include <tinydb/storage_engine.h>
#include <tinydb/unique_fd.h>
#include <tinydb/wal.h>

#include "btree/b_plus_tree.h"
#include "cache/committed_page_cache.h"
#include "cache/committed_page_source.h"
#include "io/syscalls.h"
#include "txn/commit_coordinator.h"
#include "txn/contract.h"
#include "txn/database_state.h"
#include "txn/read_snapshot.h"
#include "txn/reader_gate.h"
#include "txn/state.h"
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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

/*
** STORAGE ENGINE COORDINATION
**
** DatabaseCore owns every address borrowed by a transaction and therefore
** survives StorageEngine moves. The four storage domains are:
**
**   TransactionPages       private, mutable, discardable
**   WAL                    committed state newer than the checkpoint
**   CommittedPageCache     visible immutable page versions
**   DiskManager            checkpointed file and published metadata
**
** Commit order is fixed:
**
**   freeze structure -> assign exact commit LSN -> seal page bytes
**   -> encode WAL transaction -> prepare cache/state ownership
**   -> append and fsync WAL -> drain old readers -> publish noexcept
**
** Every validation and allocation happens before WAL append. Once fsync proves
** the COMMIT record durable, publication only transfers existing ownership and
** replaces the one visible DatabaseState pointer.
*/
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

auto AcquireDatabaseLock(const std::filesystem::path &path) -> Result<UniqueFd> {
  // Ownership precedes recovery because replay and segment cleanup are writes.
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
      .visible_lsn = disk.CheckpointLsn(),
      .checkpoint_lsn = disk.CheckpointLsn(),
  });
}

auto LifecycleError(txn::DatabaseLifecycle lifecycle, txn::DatabaseOperation operation,
                    std::size_t active_transactions = 0) -> Status {
  const auto code = txn::StateStatus(lifecycle, operation, active_transactions);
  switch (code) {
    case StatusCode::Ok:
      return {};
    case StatusCode::Busy:
      return Status::Busy("database has active transactions");
    case StatusCode::NeedsRecovery:
      return Status::NeedsRecovery("database must be reopened before further access");
    case StatusCode::Corruption:
      return Status::Corruption("database handle is in the corruption state");
    case StatusCode::Closed:
      return Status::Closed("operation on a closed database handle");
    case StatusCode::IoError:
    case StatusCode::UnsupportedFormat:
    case StatusCode::InvalidArgument:
    case StatusCode::ResourceExhausted:
    case StatusCode::IndeterminateCommit:
      TINYDB_CHECK(false, "database lifecycle produced a non-admission status");
  }
  TINYDB_CHECK(false, "unknown database lifecycle status");
}

}  // namespace

namespace detail {

class DatabaseCore final {
 public:
  DatabaseCore(std::filesystem::path database_path, UniqueFd database_lock, std::unique_ptr<DiskManager> database_file,
               std::unique_ptr<cache::CommittedPageCache> page_cache,
               std::unique_ptr<cache::CommittedPageSource> page_source, std::unique_ptr<txn::ReaderGate> reader_gate,
               std::unique_ptr<Wal> write_ahead_log)
      : path(std::move(database_path)),
        lock_fd(std::move(database_lock)),
        disk(std::move(database_file)),
        cache(std::move(page_cache)),
        pages(std::move(page_source)),
        readers(std::move(reader_gate)),
        wal(std::move(write_ahead_log)) {}

  DatabaseCore(const DatabaseCore &) = delete;
  auto operator=(const DatabaseCore &) -> DatabaseCore & = delete;

  ~DatabaseCore() { ReleaseResources(); }

  void ReleaseTransaction() noexcept {
    auto lock = std::lock_guard(lifecycle_mutex);
    TINYDB_CHECK(active_transactions != 0, "database transaction count underflow");
    --active_transactions;
  }

  void NeedsRecovery() noexcept {
    auto lock = std::lock_guard(lifecycle_mutex);
    if (lifecycle == txn::DatabaseLifecycle::Open || lifecycle == txn::DatabaseLifecycle::CheckpointDegraded) {
      lifecycle = txn::DatabaseLifecycle::NeedsRecovery;
    }
  }

  auto Commit(txn::TransactionPages &transaction, BPlusTree &tree,
              txn::TransactionState &transaction_state) -> Result<TransactionCommitInfo> {
    auto coordinator = txn::CommitCoordinator(wal.get(), cache.get(), disk.get(), readers.get());
    auto result = coordinator.Commit(transaction, tree, transaction_state);
    if (!result && (result.error().Code() == StatusCode::IndeterminateCommit ||
                    result.error().Code() == StatusCode::NeedsRecovery)) {
      NeedsRecovery();
    }
    return result;
  }

  auto CheckpointLocked() -> Status {
    /*
    ** CHECKPOINT ORDER
    **
    **   extend file -> write captured dirty pages -> write superblocks
    **   -> fsync database -> reset WAL -> mark frames checkpointed
    **   -> publish advanced checkpoint frontier
    **
    ** Commit success never depends on this maintenance path. A failure leaves
    ** WAL authoritative and moves the live handle to CheckpointDegraded.
    */
    const auto state = readers->CurrentState();
    auto next = *state;
    next.checkpoint_lsn = state->visible_lsn;
    auto checkpointed_state = std::make_shared<const txn::DatabaseState>(std::move(next));
    if (auto status = disk->EnsurePageCount(state->high_water_page_id); !status.Ok()) {
      return status;
    }
    auto dirty = cache->DirtyPages();
    for (const auto &page : dirty) {
      if (auto status = disk->WritePage(page.Id(), page.Data().data()); !status.Ok()) {
        return status;
      }
    }
    disk->AdvanceCheckpoint(state->visible_lsn);
    if (auto status = disk->Checkpoint(); !status.Ok()) {
      return status;
    }
    if (auto status = disk->Sync(); !status.Ok()) {
      return status;
    }
    if (auto status = wal->Reset(); !status.Ok()) {
      return status;
    }
    cache->MarkCheckpointed(state->visible_lsn);
    auto publication = readers->BeginPublication();
    publication.Publish(std::move(checkpointed_state));
    return {};
  }

  auto Close() -> Status {
    auto lifecycle_lock = std::unique_lock(lifecycle_mutex);
    if (lifecycle == txn::DatabaseLifecycle::Closed) {
      return {};
    }
    if (active_transactions != 0) {
      return Status::Busy("database has active transactions");
    }

    // Holding the lifecycle mutex prevents new admission while the writer
    // permit and final checkpoint are acquired.
    auto writer = std::unique_lock(writer_mutex);
    if (lifecycle == txn::DatabaseLifecycle::Open || lifecycle == txn::DatabaseLifecycle::CheckpointDegraded) {
      if (auto status = CheckpointLocked(); !status.Ok()) {
        lifecycle = txn::DatabaseLifecycle::CheckpointDegraded;
        return status;
      }
    }
    lifecycle = txn::DatabaseLifecycle::Closed;
    ReleaseResources();
    return {};
  }

  void ReleaseResources() noexcept {
    readers.reset();
    pages.reset();
    cache.reset();
    disk.reset();
    wal.reset();
    lock_fd = UniqueFd{};
  }

  std::filesystem::path path;
  mutable std::mutex lifecycle_mutex;
  txn::DatabaseLifecycle lifecycle{txn::DatabaseLifecycle::Open};
  std::size_t active_transactions{0};
  std::mutex writer_mutex;

  UniqueFd lock_fd;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<cache::CommittedPageCache> cache;
  std::unique_ptr<cache::CommittedPageSource> pages;
  std::unique_ptr<txn::ReaderGate> readers;
  std::unique_ptr<Wal> wal;
};

}  // namespace detail

struct ReadTransaction::Impl final {
  Impl(std::shared_ptr<detail::DatabaseCore> database, txn::ReadSnapshot read_snapshot)
      : core(std::move(database)), snapshot(std::move(read_snapshot)) {}

  ~Impl() { Release(); }

  void Release() noexcept {
    if (!active) {
      return;
    }
    // Reader admission must be released before taking lifecycle_mutex. A new
    // BeginRead may hold that mutex while waiting behind a pending publisher;
    // reversing these two releases would make the publisher wait on us while
    // we wait on the new reader.
    snapshot.reset();
    active = false;
    core->ReleaseTransaction();
  }

  std::shared_ptr<detail::DatabaseCore> core;
  std::optional<txn::ReadSnapshot> snapshot;
  bool active{true};
};

struct WriteTransaction::Impl final {
  Impl(std::shared_ptr<detail::DatabaseCore> database, std::unique_lock<std::mutex> writer_permit,
       std::unique_ptr<txn::TransactionPages> private_pages, std::unique_ptr<BPlusTree> private_tree)
      : core(std::move(database)),
        writer(std::move(writer_permit)),
        transaction(std::move(private_pages)),
        tree(std::move(private_tree)) {}

  ~Impl() { Abort(); }

  void Release() noexcept {
    if (!active) {
      return;
    }
    active = false;
    core->ReleaseTransaction();
    if (writer.owns_lock()) {
      writer.unlock();
    }
  }

  void Abort() noexcept {
    if (!active) {
      return;
    }
    transaction->Abort();
    state = txn::TransactionState::Aborted;
    Release();
  }

  std::shared_ptr<detail::DatabaseCore> core;
  std::unique_lock<std::mutex> writer;
  std::unique_ptr<txn::TransactionPages> transaction;
  std::unique_ptr<BPlusTree> tree;
  txn::TransactionState state{txn::TransactionState::Active};
  bool active{true};
};

ReadTransaction::ReadTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReadTransaction::ReadTransaction(ReadTransaction &&) noexcept = default;
auto ReadTransaction::operator=(ReadTransaction &&) noexcept -> ReadTransaction & = default;
ReadTransaction::~ReadTransaction() = default;

auto ReadTransaction::Get(std::string_view key) -> Result<std::optional<std::string>> {
  if (impl_ == nullptr || !impl_->active) {
    return std::unexpected(Status::Closed("Get on an inactive read transaction"));
  }
  return impl_->snapshot->Get(key);
}

WriteTransaction::WriteTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WriteTransaction::WriteTransaction(WriteTransaction &&) noexcept = default;
auto WriteTransaction::operator=(WriteTransaction &&) noexcept -> WriteTransaction & = default;
WriteTransaction::~WriteTransaction() = default;

auto WriteTransaction::Get(std::string_view key) -> Result<std::optional<std::string>> {
  if (impl_ == nullptr || !impl_->active) {
    return std::unexpected(Status::Closed("Get on an inactive write transaction"));
  }
  return impl_->tree->Get(key);
}

auto WriteTransaction::Put(std::string_view key, std::string_view value) -> Status {
  if (impl_ == nullptr || !impl_->active) {
    return Status::Closed("Put on an inactive write transaction");
  }
  if (txn::ValidateKeySize(key.size()) != StatusCode::Ok || key.size() + value.size() > MAX_ENTRY_BYTES) {
    return Status::InvalidArgument("key or value exceeds the current encoded entry limit");
  }
  auto status = impl_->transaction->ChargeValueBytes(value.size());
  if (status.Ok()) {
    status = impl_->tree->Put(key, value);
  }
  if (!status.Ok() && status.Code() != StatusCode::InvalidArgument) {
    impl_->Abort();
  }
  return status;
}

auto WriteTransaction::Delete(std::string_view key) -> Status {
  if (impl_ == nullptr || !impl_->active) {
    return Status::Closed("Delete on an inactive write transaction");
  }
  if (txn::ValidateKeySize(key.size()) != StatusCode::Ok) {
    return Status::InvalidArgument("key exceeds the maximum key size");
  }
  auto status = impl_->tree->Remove(key);
  if (!status.Ok() && status.Code() != StatusCode::InvalidArgument) {
    impl_->Abort();
  }
  return status;
}

auto WriteTransaction::Commit() -> Result<TransactionCommitInfo> {
  if (impl_ == nullptr || !impl_->active) {
    return std::unexpected(Status::Closed("Commit on an inactive write transaction"));
  }
  auto result = impl_->core->Commit(*impl_->transaction, *impl_->tree, impl_->state);
  impl_->Release();
  return result;
}

void WriteTransaction::Abort() noexcept {
  if (impl_ != nullptr) {
    impl_->Abort();
  }
}

auto StorageEngine::Open(const std::filesystem::path &path) -> Result<StorageEngine> {
  /*
  ** OPEN ORDER
  **
  ** Process ownership comes first. Recovery completes before DiskManager
  ** selects superblocks and before any cache can retain page bytes.
  */
  auto lock = AcquireDatabaseLock(path);
  if (!lock) {
    return std::unexpected(lock.error());
  }
  const auto wal_path = Wal::PathFor(path);
  if (auto status = Wal::Recover(path, wal_path); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto disk_result = DiskManager::Open(path);
  if (!disk_result) {
    return std::unexpected(disk_result.error());
  }
  auto disk = std::make_unique<DiskManager>(*std::move(disk_result));
  auto cache = std::make_unique<cache::CommittedPageCache>(disk.get(), CACHE_TARGET_BYTES, disk->CheckpointLsn());
  auto pages = std::make_unique<cache::CommittedPageSource>(cache.get());
  auto readers = std::make_unique<txn::ReaderGate>(InitialState(*disk));
  auto wal_result = Wal::Open(wal_path, disk->Uuid(), disk->TransactionId() + 1, disk->CheckpointLsn() + 1);
  if (!wal_result) {
    return std::unexpected(wal_result.error());
  }
  auto wal = std::make_unique<Wal>(*std::move(wal_result));
  auto core = std::make_shared<detail::DatabaseCore>(path, std::move(*lock), std::move(disk), std::move(cache),
                                                     std::move(pages), std::move(readers), std::move(wal));
  auto engine = StorageEngine(std::move(core));

  // Bootstrap allocates the first root through the same public write path as
  // every application transaction.
  if (engine.core_->disk->GetRootPageId() == HEADER_PAGE_ID) {
    auto transaction = engine.BeginWrite();
    if (!transaction) {
      return std::unexpected(transaction.error());
    }
    const auto committed = transaction->Commit();
    if (!committed) {
      return std::unexpected(committed.error());
    }
  }
  if (auto status = engine.CheckIntegrity(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return engine;
}

StorageEngine::~StorageEngine() { CloseBestEffort(); }

void StorageEngine::CloseBestEffort() noexcept {
  if (core_ == nullptr) {
    return;
  }
  if (const auto status = Close(); !status.Ok() && status.Code() != StatusCode::Busy) {
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", core_->path.c_str(), status.ToString().c_str());
  }
}

auto StorageEngine::BeginRead() -> Result<ReadTransaction> {
  if (core_ == nullptr) {
    return std::unexpected(Status::Closed("BeginRead on a moved-from database"));
  }
  auto lock = std::lock_guard(core_->lifecycle_mutex);
  if (auto status = LifecycleError(core_->lifecycle, txn::DatabaseOperation::Read); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto snapshot = txn::ReadSnapshot::Begin(core_->readers.get(), core_->pages.get());
  auto impl = std::make_unique<ReadTransaction::Impl>(core_, std::move(snapshot));
  ++core_->active_transactions;
  return ReadTransaction(std::move(impl));
}

auto StorageEngine::BeginWrite() -> Result<WriteTransaction> {
  if (core_ == nullptr) {
    return std::unexpected(Status::Closed("BeginWrite on a moved-from database"));
  }
  auto lifecycle_lock = std::lock_guard(core_->lifecycle_mutex);
  if (auto status = LifecycleError(core_->lifecycle, txn::DatabaseOperation::Write); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto writer = std::unique_lock(core_->writer_mutex, std::try_to_lock);
  if (!writer.owns_lock()) {
    return std::unexpected(Status::Busy("another write transaction is active"));
  }

  /*
  ** CHECKPOINT BEFORE THE NEXT WRITE
  **
  ** Maintenance never runs between WAL durability and returning CommitInfo.
  ** If the retained log crossed its target, the next writer attempts a
  ** checkpoint while it is still in a definite pre-transaction region.
  */
  if (core_->wal->SizeBytes() > CHECKPOINT_THRESHOLD_BYTES) {
    const auto checkpoint = core_->CheckpointLocked();
    if (checkpoint.Code() == StatusCode::NeedsRecovery || checkpoint.Code() == StatusCode::IndeterminateCommit) {
      core_->lifecycle = txn::DatabaseLifecycle::NeedsRecovery;
      return std::unexpected(checkpoint);
    }
    core_->lifecycle = checkpoint.Ok() ? txn::DatabaseLifecycle::Open : txn::DatabaseLifecycle::CheckpointDegraded;
  }

  const auto base = core_->readers->CurrentState();
  auto transaction = txn::TransactionPages::Begin(core_->pages.get(), *base, WRITE_TRANSACTION_LIMIT_BYTES);
  if (!transaction) {
    return std::unexpected(transaction.error());
  }
  auto private_pages = std::make_unique<txn::TransactionPages>(std::move(*transaction));
  auto root_page_id = base->root_page_id;
  if (root_page_id == HEADER_PAGE_ID) {
    auto root = private_pages->Allocate();
    if (!root) {
      private_pages->Abort();
      return std::unexpected(root.error());
    }
    root_page_id = root->Id();
    root = PageHandle{};
  }
  auto tree = BPlusTree::Open(private_pages.get(), root_page_id);
  if (!tree) {
    private_pages->Abort();
    return std::unexpected(tree.error());
  }
  auto private_tree = std::make_unique<BPlusTree>(std::move(*tree));
  auto impl = std::make_unique<WriteTransaction::Impl>(core_, std::move(writer), std::move(private_pages),
                                                       std::move(private_tree));
  ++core_->active_transactions;
  return WriteTransaction(std::move(impl));
}

auto StorageEngine::Put(std::string_view key, std::string_view value) -> Status {
  auto transaction = BeginWrite();
  if (!transaction) {
    return transaction.error();
  }
  if (auto status = transaction->Put(key, value); !status.Ok()) {
    return status;
  }
  const auto committed = transaction->Commit();
  return committed ? Status{} : committed.error();
}

auto StorageEngine::Get(std::string_view key) -> Result<std::optional<std::string>> {
  auto transaction = BeginRead();
  if (!transaction) {
    return std::unexpected(transaction.error());
  }
  return transaction->Get(key);
}

auto StorageEngine::Remove(std::string_view key) -> Status {
  auto transaction = BeginWrite();
  if (!transaction) {
    return transaction.error();
  }
  if (auto status = transaction->Delete(key); !status.Ok()) {
    return status;
  }
  const auto committed = transaction->Commit();
  return committed ? Status{} : committed.error();
}

auto StorageEngine::Scan(std::string_view start,
                         std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>> {
  auto transaction = BeginRead();
  if (!transaction) {
    return std::unexpected(transaction.error());
  }
  auto cursor = transaction->impl_->snapshot->Seek(start);
  if (!cursor) {
    return std::unexpected(cursor.error());
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
  auto read = BeginRead();
  if (!read) {
    return read.error();
  }
  const auto &state = read->impl_->snapshot->State();
  auto transaction = txn::TransactionPages::Begin(core_->pages.get(), state, WRITE_TRANSACTION_LIMIT_BYTES);
  if (!transaction) {
    return transaction.error();
  }
  auto free_pages = std::unordered_set<page_id_t>{};
  for (const auto &extent : transaction->FreeExtents()) {
    for (page_id_t page_id = extent.first_page_id; page_id < extent.first_page_id + extent.page_count; ++page_id) {
      free_pages.insert(page_id);
    }
  }
  const auto allocator_pages =
      std::unordered_set<page_id_t>(transaction->AllocatorPageIds().begin(), transaction->AllocatorPageIds().end());
  return BPlusTree::CheckIntegrity(core_->pages.get(), state.root_page_id, state.high_water_page_id, free_pages,
                                   allocator_pages);
}

auto StorageEngine::Close() -> Status {
  if (core_ == nullptr) {
    return {};
  }
  return core_->Close();
}

}  // namespace tinydb
