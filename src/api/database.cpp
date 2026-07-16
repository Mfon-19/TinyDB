#include <tinydb/database.h>

#include "io/unique_fd.h"
#include "storage/disk_manager.h"
#include "util/check.h"
#include "wal/wal.h"

#include "btree/b_plus_tree.h"
#include "backup/backup_manager.h"
#include "cache/committed_page_cache.h"
#include "cache/committed_page_source.h"
#include "checkpoint/checkpoint_manager.h"
#include "io/file_io.h"
#include "io/syscalls.h"
#include "recovery/recovery.h"
#include "txn/commit_coordinator.h"
#include "txn/contract.h"
#include "txn/database_state.h"
#include "txn/read_snapshot.h"
#include "txn/reader_gate.h"
#include "txn/state.h"
#include "txn/transaction_pages.h"
#include "verify/verifier.h"
#include "wal/wal_codec.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
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
#include <utility>
#include <vector>

namespace tinydb {
namespace {

/*
** DATABASE COORDINATION
**
** DatabaseCore owns every address borrowed by a transaction and therefore
** survives Database moves. The four storage domains are:
**
**   TransactionPages       private, mutable, discardable
**   WAL                    committed state newer than the checkpoint
**   CommittedPageCache     visible immutable page versions
**   DiskManager            checkpointed file and durable metadata
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
using io::ErrnoStatus;
using io::SyncParentDirectory;

auto ValidateOptions(const Options &options) -> Status {
  if (options.page_cache_bytes < PAGE_SIZE) {
    return Status::InvalidArgument("page cache must hold at least one database page");
  }
  if (options.max_write_transaction_bytes < PAGE_SIZE) {
    return Status::InvalidArgument("write transaction budget must hold at least one database page");
  }
  if (options.wal_segment_bytes <= wal_format::HEADER_BYTES) {
    return Status::InvalidArgument("WAL segment size must exceed the encoded segment header");
  }
  const auto &checkpoint = options.checkpoint;
  if (checkpoint.wal_trigger_bytes == 0 || checkpoint.dirty_trigger_bytes == 0 ||
      checkpoint.hard_wal_bytes < checkpoint.wal_trigger_bytes ||
      checkpoint.hard_dirty_bytes < checkpoint.dirty_trigger_bytes || checkpoint.failures_before_backpressure == 0 ||
      checkpoint.maximum_age <= std::chrono::milliseconds::zero()) {
    return Status::InvalidArgument("checkpoint thresholds must be positive and hard limits must cover soft limits");
  }
  return {};
}

auto CheckpointPolicy(const CheckpointOptions &options) -> checkpoint::Policy {
  return checkpoint::Policy{
      .wal_trigger_bytes = options.wal_trigger_bytes,
      .dirty_trigger_bytes = options.dirty_trigger_bytes,
      .hard_wal_bytes = options.hard_wal_bytes,
      .hard_dirty_bytes = options.hard_dirty_bytes,
      .failures_before_backpressure = options.failures_before_backpressure,
      .maximum_age = options.maximum_age,
  };
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

auto KeyRange::All() -> KeyRange { return {}; }

auto KeyRange::From(BytesView lower) -> KeyRange { return KeyRange(Bytes(lower), std::nullopt); }

auto KeyRange::Until(BytesView upper) -> KeyRange { return KeyRange(std::nullopt, Bytes(upper)); }

auto KeyRange::Between(BytesView lower, BytesView upper) -> KeyRange { return KeyRange(Bytes(lower), Bytes(upper)); }

auto KeyRange::Prefix(BytesView prefix) -> KeyRange {
  auto lower = Bytes(prefix);
  auto upper = lower;

  // Increment the last byte that is not 0xff and discard its suffix. Under
  // unsigned lexicographic order this is the least key greater than every key
  // beginning with prefix. An all-0xff prefix has no finite successor.
  for (auto index = upper.size(); index != 0; --index) {
    const auto byte = static_cast<unsigned char>(upper[index - 1]);
    if (byte == 0xffU) {
      continue;
    }
    upper[index - 1] = static_cast<char>(byte + 1U);
    upper.resize(index);
    return KeyRange(std::move(lower), std::move(upper));
  }
  return KeyRange(std::move(lower), std::nullopt);
}

auto KeyRange::Lower() const -> std::optional<BytesView> {
  return lower_ ? std::optional<BytesView>{*lower_} : std::nullopt;
}

auto KeyRange::Upper() const -> std::optional<BytesView> {
  return upper_ ? std::optional<BytesView>{*upper_} : std::nullopt;
}

namespace detail {

class DatabaseCore final {
 public:
  DatabaseCore(std::filesystem::path database_path, UniqueFd database_lock, std::unique_ptr<DiskManager> database_file,
               std::unique_ptr<cache::CommittedPageCache> page_cache,
               std::unique_ptr<cache::CommittedPageSource> page_source, std::unique_ptr<txn::ReaderGate> reader_gate,
               std::unique_ptr<Wal> write_ahead_log, Options database_options)
      : path(std::move(database_path)),
        options(std::move(database_options)),
        lock_fd(std::move(database_lock)),
        disk(std::move(database_file)),
        cache(std::move(page_cache)),
        pages(std::move(page_source)),
        readers(std::move(reader_gate)),
        wal(std::move(write_ahead_log)),
        checkpoints(std::make_unique<checkpoint::Manager>(disk.get(), cache.get(), readers.get(), wal.get(),
                                                          CheckpointPolicy(options.checkpoint))) {}

  DatabaseCore(const DatabaseCore &) = delete;
  auto operator=(const DatabaseCore &) -> DatabaseCore & = delete;

  ~DatabaseCore() { ReleaseResources(); }

  class MaintenancePermit final {
   public:
    MaintenancePermit(const MaintenancePermit &) = delete;
    auto operator=(const MaintenancePermit &) -> MaintenancePermit & = delete;
    MaintenancePermit(MaintenancePermit &&other) noexcept : core_(std::exchange(other.core_, nullptr)) {}
    auto operator=(MaintenancePermit &&) -> MaintenancePermit & = delete;
    ~MaintenancePermit() {
      if (core_ != nullptr) {
        core_->ReleaseMaintenance();
      }
    }

   private:
    explicit MaintenancePermit(DatabaseCore *core) : core_(core) {}
    DatabaseCore *core_;

    friend class DatabaseCore;
  };

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

  void ReleaseMaintenance() noexcept {
    auto lock = std::lock_guard(lifecycle_mutex);
    TINYDB_CHECK(active_maintenance != 0, "database maintenance count underflow");
    --active_maintenance;
  }

  auto AdmitMaintenance(txn::DatabaseOperation operation) -> Result<MaintenancePermit> {
    auto lock = std::lock_guard(lifecycle_mutex);
    if (auto status = LifecycleError(lifecycle, operation); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    ++active_maintenance;
    return MaintenancePermit(this);
  }

  auto Commit(txn::TransactionPages &transaction, BPlusTree &tree,
              txn::TransactionState &transaction_state) -> Result<CommitInfo> {
    auto coordinator = txn::CommitCoordinator(wal.get(), cache.get(), readers.get());
    auto result = coordinator.Commit(transaction, tree, transaction_state);
    if (!result && (result.error().Code() == StatusCode::IndeterminateCommit ||
                    result.error().Code() == StatusCode::NeedsRecovery)) {
      NeedsRecovery();
    }
    return result;
  }

  auto Checkpoint() -> Status {
    auto admission = AdmitMaintenance(txn::DatabaseOperation::Checkpoint);
    if (!admission) {
      return admission.error();
    }

    const auto status = checkpoints->Checkpoint();
    ObserveCheckpoint(status);
    return status;
  }

  void ObserveCheckpoint(const Status &status) noexcept {
    auto lock = std::lock_guard(lifecycle_mutex);
    if (lifecycle == txn::DatabaseLifecycle::Open || lifecycle == txn::DatabaseLifecycle::CheckpointDegraded) {
      if (status.Code() == StatusCode::NeedsRecovery || status.Code() == StatusCode::IndeterminateCommit) {
        lifecycle = txn::DatabaseLifecycle::NeedsRecovery;
      } else if (status.Code() == StatusCode::Corruption) {
        lifecycle = txn::DatabaseLifecycle::Corrupt;
      } else {
        lifecycle = status.Ok() ? txn::DatabaseLifecycle::Open : txn::DatabaseLifecycle::CheckpointDegraded;
      }
    }
  }

  /*
  ** CheckpointAndFreeze owns both checkpoint serialization and the cache/state
  ** publication lock through the copy. Read transactions remain admitted and
  ** a writer may continue private preparation and WAL synchronization, but it
  ** cannot publish a new visible state until the backup snapshot is complete.
  */
  auto CreateBackup(const std::filesystem::path &destination) -> Status {
    auto admission = AdmitMaintenance(txn::DatabaseOperation::Backup);
    if (!admission) {
      return admission.error();
    }

    auto frozen = checkpoints->CheckpointAndFreeze();
    if (!frozen) {
      ObserveCheckpoint(frozen.error());
      return frozen.error();
    }
    ObserveCheckpoint({});
    return backup::Create(*disk, destination, options.page_cache_bytes, options.max_write_transaction_bytes);
  }

  auto MaybeCheckpoint() -> Status {
    if (checkpoints->ShouldCheckpoint()) {
      const auto status = Checkpoint();
      if (status.Code() == StatusCode::NeedsRecovery || status.Code() == StatusCode::IndeterminateCommit ||
          status.Code() == StatusCode::Corruption || status.Code() == StatusCode::Closed) {
        return status;
      }
    }
    return checkpoints->WriteAdmissionStatus();
  }

  /*
  ** Required state: Open has finished recovery and no caller can yet observe
  ** this core. Validate every reachable tree and allocator page without
  ** publishing, checkpointing, or changing lifecycle state.
  */
  auto CheckIntegrity() -> Status {
    auto snapshot = txn::ReadSnapshot::Begin(readers.get(), pages.get());
    return verify::Snapshot(pages.get(), snapshot.State(), options.max_write_transaction_bytes);
  }

  /*
  ** lifecycle_mutex excludes Close while subsystem snapshots are collected.
  ** Each subsystem supplies its own coherent counters; the result is
  ** diagnostic and does not claim one transactional instant across counters.
  */
  auto Statistics() const -> Result<DatabaseStats> {
    auto lock = std::lock_guard(lifecycle_mutex);
    if (auto status = LifecycleError(lifecycle, txn::DatabaseOperation::Stats); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    const auto state = readers->CurrentState();
    const auto reader_stats = readers->Stats();
    const auto cache_stats = cache->Stats();
    const auto checkpoint_stats = checkpoints->GetStats();
    auto result = DatabaseStats{
        .transaction_id = state->transaction_id,
        .visible_lsn = state->visible_lsn,
        .checkpoint_lsn = state->checkpoint_lsn,
        .wal_bytes = wal->SizeBytes(),
        .active_readers = reader_stats.active_readers,
        .publication_pending = reader_stats.publication_pending,
        .oldest_reader_age = std::nullopt,
        .cache_target_bytes = cache_stats.target_bytes,
        .cache_resident_bytes = cache_stats.resident_bytes,
        .cache_resident_pages = cache_stats.resident_pages,
        .cache_pinned_pages = cache_stats.pinned_pages,
        .dirty_pages = cache_stats.dirty_pages,
        .consecutive_checkpoint_failures = checkpoint_stats.consecutive_failures,
        .checkpoint_requested = checkpoint_stats.checkpoint_requested,
    };
    if (reader_stats.oldest_reader_age) {
      result.oldest_reader_age = std::chrono::duration_cast<std::chrono::milliseconds>(*reader_stats.oldest_reader_age);
    }
    return result;
  }

  auto Close() -> Status {
    auto lifecycle_lock = std::unique_lock(lifecycle_mutex);
    if (lifecycle == txn::DatabaseLifecycle::Closed) {
      return {};
    }
    if (active_transactions != 0 || active_maintenance != 0) {
      return Status::Busy("database has active transactions or maintenance");
    }

    // Holding the lifecycle mutex prevents new admission while the writer
    // permit and final checkpoint are acquired.
    auto writer = std::unique_lock(writer_mutex);
    if (lifecycle == txn::DatabaseLifecycle::Open || lifecycle == txn::DatabaseLifecycle::CheckpointDegraded) {
      if (auto status = checkpoints->Checkpoint(); !status.Ok()) {
        lifecycle = txn::DatabaseLifecycle::CheckpointDegraded;
        return status;
      }
    }
    lifecycle = txn::DatabaseLifecycle::Closed;
    ReleaseResources();
    return {};
  }

  void ReleaseResources() noexcept {
    checkpoints.reset();
    readers.reset();
    pages.reset();
    cache.reset();
    disk.reset();
    wal.reset();
    lock_fd = UniqueFd{};
  }

  std::filesystem::path path;
  Options options;
  mutable std::mutex lifecycle_mutex;
  txn::DatabaseLifecycle lifecycle{txn::DatabaseLifecycle::Open};
  std::size_t active_transactions{0};
  std::size_t active_maintenance{0};
  std::mutex writer_mutex;

  UniqueFd lock_fd;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<cache::CommittedPageCache> cache;
  std::unique_ptr<cache::CommittedPageSource> pages;
  std::unique_ptr<txn::ReaderGate> readers;
  std::unique_ptr<Wal> wal;
  std::unique_ptr<checkpoint::Manager> checkpoints;
};

}  // namespace detail

struct Cursor::Impl final {
  Impl(std::shared_ptr<detail::DatabaseCore> database, txn::SnapshotCursor snapshot_cursor, KeyRange key_range)
      : core(std::move(database)), cursor(std::move(snapshot_cursor)), range(std::move(key_range)) {}

  ~Impl() { Release(); }

  void Activate() noexcept { active = true; }

  void Release() noexcept {
    if (!active) {
      return;
    }
    // Release the page lease and shared reader admission before taking the
    // lifecycle mutex in ReleaseTransaction. This preserves the same lock
    // ordering as ReadTransaction destruction.
    cursor.reset();
    active = false;
    core->ReleaseTransaction();
  }

  auto Valid() const -> bool {
    if (!active || !cursor->Valid()) {
      return false;
    }
    const auto upper = range.Upper();
    return !upper || txn::BytewiseLess{}(cursor->Key(), *upper);
  }

  auto First() -> Status {
    const auto lower = range.Lower();
    return lower ? cursor->Seek(*lower) : cursor->First();
  }

  auto Seek(std::string_view key) -> Status {
    const auto lower = range.Lower();
    if (lower && txn::BytewiseLess{}(key, *lower)) {
      key = *lower;
    }
    return cursor->Seek(key);
  }

  std::shared_ptr<detail::DatabaseCore> core;
  std::optional<txn::SnapshotCursor> cursor;
  KeyRange range;
  bool active{false};
};

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
ReadTransaction::~ReadTransaction() = default;

auto ReadTransaction::Get(BytesView key) -> Result<std::optional<Bytes>> {
  if (impl_ == nullptr || !impl_->active) {
    return std::unexpected(Status::Closed("Get on an inactive read transaction"));
  }
  return impl_->snapshot->Get(key);
}

auto ReadTransaction::Scan(KeyRange range) -> Result<Cursor> {
  if (impl_ == nullptr || !impl_->active) {
    return std::unexpected(Status::Closed("Scan on an inactive read transaction"));
  }
  auto snapshot_cursor = range.Lower() ? impl_->snapshot->Seek(*range.Lower()) : impl_->snapshot->First();
  if (!snapshot_cursor) {
    return std::unexpected(snapshot_cursor.error());
  }

  auto cursor_impl = std::make_unique<Cursor::Impl>(impl_->core, std::move(*snapshot_cursor), std::move(range));
  {
    auto lock = std::lock_guard(impl_->core->lifecycle_mutex);
    TINYDB_CHECK(impl_->core->lifecycle != txn::DatabaseLifecycle::Closed,
                 "active read transaction belongs to a closed database");
    ++impl_->core->active_transactions;
  }
  cursor_impl->Activate();
  return Cursor(std::move(cursor_impl));
}

Cursor::Cursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Cursor::Cursor(Cursor &&) noexcept = default;
Cursor::~Cursor() = default;

auto Cursor::First() -> Status {
  if (impl_ == nullptr || !impl_->active) {
    return Status::Closed("First on an inactive cursor");
  }
  return impl_->First();
}

auto Cursor::Seek(BytesView key) -> Status {
  if (impl_ == nullptr || !impl_->active) {
    return Status::Closed("Seek on an inactive cursor");
  }
  return impl_->Seek(key);
}

auto Cursor::Next() -> Status {
  if (impl_ == nullptr || !impl_->active) {
    return Status::Closed("Next on an inactive cursor");
  }
  if (!impl_->Valid()) {
    return Status::InvalidArgument("Next requires a valid cursor position");
  }
  return impl_->cursor->Next();
}

auto Cursor::Valid() const -> bool { return impl_ != nullptr && impl_->Valid(); }

auto Cursor::Key() const -> BytesView {
  TINYDB_CHECK(Valid(), "Key requires a valid cursor position");
  return impl_->cursor->Key();
}

auto Cursor::ValueSize() const -> std::uint64_t {
  TINYDB_CHECK(Valid(), "ValueSize requires a valid cursor position");
  return impl_->cursor->ValueSize();
}

auto Cursor::CopyValue() const -> Result<Bytes> {
  if (!Valid()) {
    return std::unexpected(Status::InvalidArgument("CopyValue requires a valid cursor position"));
  }
  return impl_->cursor->CopyValue();
}

WriteTransaction::WriteTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WriteTransaction::WriteTransaction(WriteTransaction &&) noexcept = default;
WriteTransaction::~WriteTransaction() = default;

auto WriteTransaction::Get(BytesView key) -> Result<std::optional<Bytes>> {
  if (impl_ == nullptr || !impl_->active) {
    return std::unexpected(Status::Closed("Get on an inactive write transaction"));
  }
  return impl_->tree->Get(key);
}

auto WriteTransaction::Put(BytesView key, BytesView value) -> Status {
  if (impl_ == nullptr || !impl_->active) {
    return Status::Closed("Put on an inactive write transaction");
  }
  if (txn::ValidateKeySize(key.size()) != StatusCode::Ok || txn::ValidateValueSize(value.size()) != StatusCode::Ok) {
    return Status::InvalidArgument("key or value exceeds its configured size limit");
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

auto WriteTransaction::Delete(BytesView key) -> Status {
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

auto WriteTransaction::Commit() && -> Result<CommitInfo> {
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

struct Database::Impl final {
  explicit Impl(std::shared_ptr<detail::DatabaseCore> database_core) : core(std::move(database_core)) {}

  std::shared_ptr<detail::DatabaseCore> core;
};

Database::Database(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Database::Database(Database &&) noexcept = default;

auto Database::Open(const std::filesystem::path &path, Options options) -> Result<Database> {
  /*
  ** OPEN ORDER
  **
  ** Options are rejected without touching the filesystem. Process ownership
  ** then precedes recovery because replay and segment cleanup mutate durable
  ** files. Recovery completes before DiskManager selects superblocks and
  ** before any cache can retain page bytes.
  */
  if (auto status = ValidateOptions(options); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto lock = AcquireDatabaseLock(path);
  if (!lock) {
    return std::unexpected(lock.error());
  }
  const auto wal_path = Wal::PathFor(path);
  if (auto status = recovery::Recover(path, wal_path); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto disk_result = DiskManager::Open(path);
  if (!disk_result) {
    return std::unexpected(disk_result.error());
  }
  auto disk = std::make_unique<DiskManager>(*std::move(disk_result));
  auto cache = std::make_unique<cache::CommittedPageCache>(disk.get(), options.page_cache_bytes, disk->CheckpointLsn());
  auto pages = std::make_unique<cache::CommittedPageSource>(cache.get());
  auto readers = std::make_unique<txn::ReaderGate>(InitialState(*disk));
  auto wal_result = Wal::Open(wal_path, disk->Uuid(), disk->TransactionId() + 1, disk->CheckpointLsn() + 1,
                              options.wal_segment_bytes);
  if (!wal_result) {
    return std::unexpected(wal_result.error());
  }
  auto wal = std::make_unique<Wal>(*std::move(wal_result));
  auto core = std::make_shared<detail::DatabaseCore>(path, std::move(*lock), std::move(disk), std::move(cache),
                                                     std::move(pages), std::move(readers), std::move(wal), options);
  auto database = Database(std::make_unique<Impl>(core));

  // Bootstrap allocates the first root through the same public write path as
  // every application transaction.
  if (core->disk->GetRootPageId() == HEADER_PAGE_ID) {
    auto transaction = database.BeginWrite();
    if (!transaction) {
      return std::unexpected(transaction.error());
    }
    const auto committed = std::move(*transaction).Commit();
    if (!committed) {
      return std::unexpected(committed.error());
    }
  }
  if (auto status = core->CheckIntegrity(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return database;
}

Database::~Database() { CloseBestEffort(); }

void Database::CloseBestEffort() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  if (const auto status = Close(); !status.Ok() && status.Code() != StatusCode::Busy) {
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", impl_->core->path.c_str(), status.ToString().c_str());
  }
}

auto Database::BeginRead() -> Result<ReadTransaction> {
  if (impl_ == nullptr) {
    return std::unexpected(Status::Closed("BeginRead on a moved-from database"));
  }
  const auto core = impl_->core;
  auto lock = std::lock_guard(core->lifecycle_mutex);
  if (auto status = LifecycleError(core->lifecycle, txn::DatabaseOperation::Read); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto snapshot = txn::ReadSnapshot::Begin(core->readers.get(), core->pages.get());
  auto transaction = std::make_unique<ReadTransaction::Impl>(core, std::move(snapshot));
  ++core->active_transactions;
  return ReadTransaction(std::move(transaction));
}

auto Database::BeginWrite() -> Result<WriteTransaction> {
  if (impl_ == nullptr) {
    return std::unexpected(Status::Closed("BeginWrite on a moved-from database"));
  }
  const auto core = impl_->core;
  {
    auto lifecycle_lock = std::lock_guard(core->lifecycle_mutex);
    if (auto status = LifecycleError(core->lifecycle, txn::DatabaseOperation::Write); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    // Reserve lifetime before releasing lifecycle_mutex for checkpoint I/O.
    // Close will report Busy instead of freeing the manager and cache between
    // this admission check and construction of the transaction handle.
    ++core->active_transactions;
  }
  struct PendingAdmission final {
    std::shared_ptr<detail::DatabaseCore> core;
    bool transferred{false};
    ~PendingAdmission() {
      if (!transferred) {
        core->ReleaseTransaction();
      }
    }
  } admission{.core = core};

  /*
  ** MAINTENANCE OPPORTUNITY
  **
  ** Checkpoint capture and I/O do not own the writer permit. A write may
  ** therefore prepare and publish while another thread writes a retained
  ** checkpoint snapshot. This admission path merely gives requested
  ** maintenance one opportunity before applying hard-pressure backoff.
  */
  if (auto status = core->MaybeCheckpoint(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  auto lifecycle_lock = std::lock_guard(core->lifecycle_mutex);
  if (auto status = LifecycleError(core->lifecycle, txn::DatabaseOperation::Write); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = core->checkpoints->WriteAdmissionStatus(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto writer = std::unique_lock(core->writer_mutex, std::try_to_lock);
  if (!writer.owns_lock()) {
    return std::unexpected(Status::Busy("another write transaction is active"));
  }

  const auto base = core->readers->CurrentState();
  auto transaction = txn::TransactionPages::Begin(core->pages.get(), *base, core->options.max_write_transaction_bytes);
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
  auto transaction_impl = std::make_unique<WriteTransaction::Impl>(core, std::move(writer), std::move(private_pages),
                                                                   std::move(private_tree));
  admission.transferred = true;
  return WriteTransaction(std::move(transaction_impl));
}

auto Database::Put(BytesView key, BytesView value) -> Status {
  auto transaction = BeginWrite();
  if (!transaction) {
    return transaction.error();
  }
  if (auto status = transaction->Put(key, value); !status.Ok()) {
    return status;
  }
  const auto committed = std::move(*transaction).Commit();
  return committed ? Status{} : committed.error();
}

auto Database::Get(BytesView key) -> Result<std::optional<Bytes>> {
  auto transaction = BeginRead();
  if (!transaction) {
    return std::unexpected(transaction.error());
  }
  return transaction->Get(key);
}

auto Database::Delete(BytesView key) -> Status {
  auto transaction = BeginWrite();
  if (!transaction) {
    return transaction.error();
  }
  if (auto status = transaction->Delete(key); !status.Ok()) {
    return status;
  }
  const auto committed = std::move(*transaction).Commit();
  return committed ? Status{} : committed.error();
}

auto Database::Checkpoint() -> Status {
  if (impl_ == nullptr) {
    return Status::Closed("Checkpoint on a moved-from database");
  }
  return impl_->core->Checkpoint();
}

auto Database::CreateBackup(const std::filesystem::path &destination) -> Status {
  if (impl_ == nullptr) {
    return Status::Closed("CreateBackup on a moved-from database");
  }
  return impl_->core->CreateBackup(destination);
}

auto Database::Stats() const -> Result<DatabaseStats> {
  if (impl_ == nullptr) {
    return std::unexpected(Status::Closed("Stats on a moved-from database"));
  }
  return impl_->core->Statistics();
}

auto Database::Close() -> Status {
  if (impl_ == nullptr) {
    return {};
  }
  return impl_->core->Close();
}

}  // namespace tinydb
