#include <tinydb/database.h>

#include "api/database_test_access.h"
#include "io/unique_fd.h"
#include "storage/disk_manager.h"
#include "util/check.h"
#include "wal/wal.h"

#include "btree/b_plus_tree.h"
#include "cache/committed_page_cache.h"
#include "checkpoint/checkpoint_manager.h"
#include "io/file_io.h"
#include "io/syscalls.h"
#include "recovery/recovery.h"
#include "txn/commit_coordinator.h"
#include "txn/contract.h"
#include "txn/database_state.h"
#include "txn/read_snapshot.h"
#include "txn/reader_gate.h"
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
#include <limits>
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

enum class Lifecycle {
  Open,
  NeedsRecovery,
  Closed,
};

auto LifecycleError(Lifecycle lifecycle, bool diagnostics = false) -> Status {
  switch (lifecycle) {
    case Lifecycle::Open:
      return {};
    case Lifecycle::NeedsRecovery:
      return diagnostics ? Status{} : Status::NeedsRecovery("database must be reopened before further access");
    case Lifecycle::Closed:
      return Status::Closed("operation on a closed database handle");
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
  struct OperationalStats final {
    std::uint64_t write_attempts{0};
    std::uint64_t committed_writes{0};
    txn::CommitTiming last_commit{};
    std::chrono::nanoseconds maximum_publication_wait{};
  };

  DatabaseCore(std::filesystem::path database_path, UniqueFd database_lock, std::unique_ptr<DiskManager> database_file,
               std::unique_ptr<cache::CommittedPageCache> page_cache, std::unique_ptr<txn::ReaderGate> reader_gate,
               std::unique_ptr<Wal> write_ahead_log, Options database_options)
      : path(std::move(database_path)),
        write_transaction_bytes(database_options.max_write_transaction_bytes),
        lock_fd(std::move(database_lock)),
        disk(std::move(database_file)),
        cache(std::move(page_cache)),
        readers(std::move(reader_gate)),
        wal(std::move(write_ahead_log)),
        checkpoints(std::make_unique<checkpoint::Manager>(disk.get(), cache.get(), readers.get(), wal.get(),
                                                          database_options.checkpoint)) {}

  DatabaseCore(const DatabaseCore &) = delete;
  auto operator=(const DatabaseCore &) -> DatabaseCore & = delete;

  ~DatabaseCore() { ReleaseResources(); }

  void NeedsRecovery() noexcept {
    auto lock = std::lock_guard(lifecycle_mutex);
    if (lifecycle == Lifecycle::Open) {
      lifecycle = Lifecycle::NeedsRecovery;
    }
  }

  auto Commit(txn::TransactionPages &transaction, BPlusTree &tree) -> Result<CommitInfo> {
    auto timing = txn::CommitTiming{};
    auto result = txn::CommitTransaction(*wal, *cache, *readers, transaction, tree, &timing);
    {
      auto lock = std::lock_guard(operational_stats_mutex);
      ++operational_stats.write_attempts;
      operational_stats.last_commit = timing;
      operational_stats.maximum_publication_wait =
          std::max(operational_stats.maximum_publication_wait, timing.publication_wait);
      if (result) {
        ++operational_stats.committed_writes;
      }
    }
    if (!result && (result.error().Code() == StatusCode::IndeterminateCommit ||
                    result.error().Code() == StatusCode::NeedsRecovery)) {
      NeedsRecovery();
    }
    return result;
  }

  auto Checkpoint() -> Status {
    auto writer = std::unique_lock(writer_mutex, std::try_to_lock);
    if (!writer.owns_lock()) {
      return Status::Busy("database has an active write transaction");
    }
    {
      auto lock = std::lock_guard(lifecycle_mutex);
      if (auto status = LifecycleError(lifecycle); !status.Ok()) {
        return status;
      }
    }
    return checkpoints->Checkpoint();
  }

  /*
  ** Verify holds an ordinary read snapshot, so publication waits while the
  ** audit follows cross-page references.  It neither takes the writer permit
  ** nor invokes checkpointing.  Corruption is returned in the report; only an
  ** environmental failure prevents a report from being produced.
  */
  auto Verify(VerifyOptions verify_options) -> Result<VerifyReport> {
    auto snapshot = [&]() -> Result<txn::ReadSnapshot> {
      auto lock = std::lock_guard(lifecycle_mutex);
      if (auto status = LifecycleError(lifecycle); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
      return txn::ReadSnapshot::Begin(readers.get(), cache.get());
    }();
    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }
    return verify::Snapshot(cache.get(), snapshot->State(), write_transaction_bytes, verify_options);
  }

  /*
  ** lifecycle_mutex excludes Close while subsystem snapshots are collected.
  ** Each subsystem supplies its own coherent counters; the result is
  ** diagnostic and does not claim one transactional instant across counters.
  */
  auto Statistics() const -> Result<DatabaseStats> {
    auto lock = std::lock_guard(lifecycle_mutex);
    if (auto status = LifecycleError(lifecycle, true); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    const auto state = readers->CurrentState();
    const auto reader_stats = readers->Stats();
    const auto cache_stats = cache->Stats();
    const auto checkpoint_stats = checkpoints->GetStats();
    auto operation_stats = OperationalStats{};
    {
      auto stats_lock = std::lock_guard(operational_stats_mutex);
      operation_stats = operational_stats;
    }
    auto result = DatabaseStats{
        .transaction_id = state->transaction_id,
        .visible_lsn = state->visible_lsn,
        .checkpoint_lsn = state->checkpoint_lsn,
        .wal_bytes = wal->SizeBytes(),
        .wal_segments = wal->SegmentCount(),
        .active_readers = reader_stats.active_readers,
        .publication_pending = reader_stats.publication_pending,
        .oldest_reader_age = std::nullopt,
        .cache_target_bytes = cache_stats.target_bytes,
        .cache_resident_bytes = cache_stats.resident_bytes,
        .cache_resident_pages = cache_stats.resident_pages,
        .cache_pinned_pages = cache_stats.pinned_pages,
        .dirty_pages = cache_stats.dirty_pages,
        .dirty_bytes = cache_stats.dirty_pages > std::numeric_limits<std::size_t>::max() / PAGE_SIZE
                           ? std::numeric_limits<std::size_t>::max()
                           : cache_stats.dirty_pages * PAGE_SIZE,
        .cache_hits = cache_stats.hits,
        .cache_misses = cache_stats.misses,
        .cache_evictions = cache_stats.evictions,
        .write_attempts = operation_stats.write_attempts,
        .committed_writes = operation_stats.committed_writes,
        .last_write_prepare = operation_stats.last_commit.prepare,
        .last_wal_sync = operation_stats.last_commit.wal_sync,
        .last_publication_wait = operation_stats.last_commit.publication_wait,
        .maximum_publication_wait = operation_stats.maximum_publication_wait,
        .consecutive_checkpoint_failures = checkpoint_stats.consecutive_failures,
        .checkpoint_requested = checkpoint_stats.checkpoint_requested,
        .checkpoint_age = std::chrono::duration_cast<std::chrono::milliseconds>(checkpoint_stats.age_since_success),
        .last_checkpoint_error = checkpoint_stats.last_error,
    };
    if (reader_stats.oldest_reader_age) {
      result.oldest_reader_age = std::chrono::duration_cast<std::chrono::milliseconds>(*reader_stats.oldest_reader_age);
    }
    return result;
  }

  auto Close() -> Status {
    // The global order is writer_mutex before lifecycle_mutex. try_to_lock
    // preserves Close's Busy contract instead of waiting for an application
    // transaction while still preventing a new writer from crossing the
    // lifecycle check.
    auto writer = std::unique_lock(writer_mutex, std::try_to_lock);
    if (!writer.owns_lock()) {
      return Status::Busy("database has an active write transaction");
    }
    auto lifecycle_lock = std::unique_lock(lifecycle_mutex);
    if (lifecycle == Lifecycle::Closed) {
      return {};
    }
    if (readers->Stats().active_readers != 0) {
      return Status::Busy("database has active readers");
    }
    if (lifecycle == Lifecycle::Open) {
      if (auto status = checkpoints->Checkpoint(); !status.Ok()) {
        return status;
      }
    }
    lifecycle = Lifecycle::Closed;
    ReleaseResources();
    return {};
  }

  void ReleaseResources() noexcept {
    checkpoints.reset();
    readers.reset();
    cache.reset();
    disk.reset();
    wal.reset();
    lock_fd = UniqueFd{};
  }

  std::filesystem::path path;
  std::size_t write_transaction_bytes;

  /*
  ** CORE LOCK ORDER
  **
  ** Code needing both the writer permit and lifecycle state acquires
  ** writer_mutex first. lifecycle_mutex protects admission counts and state;
  ** writer_mutex protects write preparation, commit, and checkpoint I/O.
  ** Close and explicit Checkpoint use a non-blocking acquisition, so an
  ** application-owned writer produces Busy rather than a wait or deadlock.
  */
  mutable std::mutex lifecycle_mutex;
  Lifecycle lifecycle{Lifecycle::Open};
  std::mutex writer_mutex;
  mutable std::mutex operational_stats_mutex;
  OperationalStats operational_stats;

  UniqueFd lock_fd;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<cache::CommittedPageCache> cache;
  std::unique_ptr<txn::ReaderGate> readers;
  std::unique_ptr<Wal> wal;
  std::unique_ptr<checkpoint::Manager> checkpoints;
};

}  // namespace detail

struct Cursor::Impl final {
  Impl(std::shared_ptr<detail::DatabaseCore> database, txn::SnapshotCursor snapshot_cursor, KeyRange key_range)
      : core(std::move(database)), cursor(std::move(snapshot_cursor)), range(std::move(key_range)) {}

  auto Valid() const -> bool {
    if (!cursor.Valid()) {
      return false;
    }
    const auto upper = range.Upper();
    return !upper || txn::BytewiseLess{}(cursor.Key(), *upper);
  }

  auto First() -> Status {
    const auto lower = range.Lower();
    return lower ? cursor.Seek(*lower) : cursor.First();
  }

  auto Seek(std::string_view key) -> Status {
    const auto lower = range.Lower();
    if (lower && txn::BytewiseLess{}(key, *lower)) {
      key = *lower;
    }
    return cursor.Seek(key);
  }

  std::shared_ptr<detail::DatabaseCore> core;
  txn::SnapshotCursor cursor;
  KeyRange range;
};

struct ReadTransaction::Impl final {
  Impl(std::shared_ptr<detail::DatabaseCore> database, txn::ReadSnapshot read_snapshot)
      : core(std::move(database)), snapshot(std::move(read_snapshot)) {}

  std::shared_ptr<detail::DatabaseCore> core;
  txn::ReadSnapshot snapshot;
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
    if (transaction == nullptr) {
      return;
    }
    tree.reset();
    transaction.reset();

    if (writer.owns_lock()) {
      writer.unlock();
    }
  }

  void Abort() noexcept {
    if (transaction == nullptr) {
      return;
    }
    transaction->Abort();
    Release();
  }

  std::shared_ptr<detail::DatabaseCore> core;
  std::unique_lock<std::mutex> writer;
  std::unique_ptr<txn::TransactionPages> transaction;
  std::unique_ptr<BPlusTree> tree;
};

ReadTransaction::ReadTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReadTransaction::ReadTransaction(ReadTransaction &&) noexcept = default;
ReadTransaction::~ReadTransaction() = default;

auto ReadTransaction::Get(BytesView key) -> Result<std::optional<Bytes>> {
  if (impl_ == nullptr) {
    return std::unexpected(Status::Closed("Get on an inactive read transaction"));
  }
  return impl_->snapshot.Get(key);
}

auto ReadTransaction::Scan(KeyRange range) -> Result<Cursor> {
  if (impl_ == nullptr) {
    return std::unexpected(Status::Closed("Scan on an inactive read transaction"));
  }
  auto snapshot_cursor = range.Lower() ? impl_->snapshot.Seek(*range.Lower()) : impl_->snapshot.First();
  if (!snapshot_cursor) {
    return std::unexpected(snapshot_cursor.error());
  }

  return Cursor(std::make_unique<Cursor::Impl>(impl_->core, std::move(*snapshot_cursor), std::move(range)));
}

Cursor::Cursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Cursor::Cursor(Cursor &&) noexcept = default;
Cursor::~Cursor() = default;

auto Cursor::First() -> Status {
  if (impl_ == nullptr) {
    return Status::Closed("First on an inactive cursor");
  }
  return impl_->First();
}

auto Cursor::Seek(BytesView key) -> Status {
  if (impl_ == nullptr) {
    return Status::Closed("Seek on an inactive cursor");
  }
  return impl_->Seek(key);
}

auto Cursor::Next() -> Status {
  if (impl_ == nullptr) {
    return Status::Closed("Next on an inactive cursor");
  }
  if (!impl_->Valid()) {
    return Status::InvalidArgument("Next requires a valid cursor position");
  }
  return impl_->cursor.Next();
}

auto Cursor::Valid() const -> bool { return impl_ != nullptr && impl_->Valid(); }

auto Cursor::Key() const -> BytesView {
  TINYDB_CHECK(Valid(), "Key requires a valid cursor position");
  return impl_->cursor.Key();
}

auto Cursor::ValueSize() const -> std::uint64_t {
  TINYDB_CHECK(Valid(), "ValueSize requires a valid cursor position");
  return impl_->cursor.ValueSize();
}

auto Cursor::CopyValue() const -> Result<Bytes> {
  if (!Valid()) {
    return std::unexpected(Status::InvalidArgument("CopyValue requires a valid cursor position"));
  }
  return impl_->cursor.CopyValue();
}

WriteTransaction::WriteTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WriteTransaction::WriteTransaction(WriteTransaction &&) noexcept = default;
WriteTransaction::~WriteTransaction() = default;

auto WriteTransaction::Get(BytesView key) -> Result<std::optional<Bytes>> {
  if (impl_ == nullptr || impl_->transaction == nullptr) {
    return std::unexpected(Status::Closed("Get on an inactive write transaction"));
  }
  return impl_->tree->Get(key);
}

auto WriteTransaction::Put(BytesView key, BytesView value) -> Status {
  if (impl_ == nullptr || impl_->transaction == nullptr) {
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
  if (impl_ == nullptr || impl_->transaction == nullptr) {
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
  if (impl_ == nullptr || impl_->transaction == nullptr) {
    return std::unexpected(Status::Closed("Commit on an inactive write transaction"));
  }
  auto result = impl_->core->Commit(*impl_->transaction, *impl_->tree);
  impl_->Release();
  return result;
}

void WriteTransaction::Abort() noexcept {
  if (impl_ != nullptr) {
    impl_->Abort();
  }
}

void DatabaseTestAccess::WaitForReadQuiescence(Database &database) noexcept {
  /* Buffered reads complete on the calling thread. */
  (void)database;
}

Database::Database(std::shared_ptr<detail::DatabaseCore> core) : core_(std::move(core)) {}
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
  auto readers = std::make_unique<txn::ReaderGate>(InitialState(*disk));
  auto wal_result = Wal::Open(wal_path, disk->Uuid(), disk->TransactionId() + 1, disk->CheckpointLsn() + 1,
                              options.wal_segment_bytes);
  if (!wal_result) {
    return std::unexpected(wal_result.error());
  }
  auto wal = std::make_unique<Wal>(*std::move(wal_result));
  auto core = std::make_shared<detail::DatabaseCore>(path, std::move(*lock), std::move(disk), std::move(cache),
                                                     std::move(readers), std::move(wal), options);
  auto database = Database(core);

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
  return database;
}

Database::~Database() {
  if (core_ != nullptr) {
    const auto status = Close();
    if (status.Ok() || status.Code() == StatusCode::Busy) {
      return;
    }
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", core_->path.c_str(), status.ToString().c_str());
  }
}

auto Database::BeginRead() -> Result<ReadTransaction> {
  if (core_ == nullptr) {
    return std::unexpected(Status::Closed("BeginRead on a moved-from database"));
  }
  const auto core = core_;
  auto lock = std::lock_guard(core->lifecycle_mutex);
  if (auto status = LifecycleError(core->lifecycle); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto snapshot = txn::ReadSnapshot::Begin(core->readers.get(), core->cache.get());
  auto transaction = std::make_unique<ReadTransaction::Impl>(core, std::move(snapshot));
  return ReadTransaction(std::move(transaction));
}

auto Database::BeginWrite() -> Result<WriteTransaction> {
  if (core_ == nullptr) {
    return std::unexpected(Status::Closed("BeginWrite on a moved-from database"));
  }
  const auto core = core_;
  auto writer = std::unique_lock(core->writer_mutex, std::try_to_lock);
  if (!writer.owns_lock()) {
    return std::unexpected(Status::Busy("another write transaction is active"));
  }
  {
    auto lifecycle_lock = std::lock_guard(core->lifecycle_mutex);
    if (auto status = LifecycleError(core->lifecycle); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }

  // The writer permit makes checkpoint capture, I/O, and frontier publication
  // one simple maintenance phase before private transaction preparation.
  if (core->checkpoints->ShouldCheckpoint()) {
    const auto status = core->checkpoints->Checkpoint();
    if (status.Code() == StatusCode::NeedsRecovery || status.Code() == StatusCode::IndeterminateCommit ||
        status.Code() == StatusCode::Corruption || status.Code() == StatusCode::Closed) {
      return std::unexpected(status);
    }
  }
  if (auto status = core->checkpoints->WriteAdmissionStatus(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  const auto base = core->readers->CurrentState();
  auto transaction = txn::TransactionPages::Begin(core->cache.get(), *base, core->write_transaction_bytes);
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
  if (core_ == nullptr) {
    return Status::Closed("Checkpoint on a moved-from database");
  }
  return core_->Checkpoint();
}

auto Database::Verify(VerifyOptions options) -> Result<VerifyReport> {
  if (core_ == nullptr) {
    return std::unexpected(Status::Closed("Verify on a moved-from database"));
  }
  return core_->Verify(options);
}

auto Database::Stats() const -> Result<DatabaseStats> {
  if (core_ == nullptr) {
    return std::unexpected(Status::Closed("Stats on a moved-from database"));
  }
  return core_->Statistics();
}

auto Database::Close() -> Status {
  if (core_ == nullptr) {
    return {};
  }
  return core_->Close();
}

}  // namespace tinydb
