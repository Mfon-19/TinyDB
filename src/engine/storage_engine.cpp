#include <tinydb/storage_engine.h>
#include <tinydb/unique_fd.h>

#include "btree/buffer_pool_page_source.h"
#include "btree/page_source.h"
#include "io/syscalls.h"

#include "btree/b_plus_tree.h"

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
#include <utility>
#include <vector>

/*
  StorageEngine wires the stack together and owns the one protocol that
  spans every layer: how a mutation becomes durable, and how the database
  file eventually catches up. This comment is the canonical statement of
  that protocol — wal.cpp, buffer_pool.cpp, and disk_manager.cpp each
  document their own slice and defer to this one for the ordering.

  The layers, and what each owns:

      StorageEngine   the ordering below; poisoning on failure
      BPlusTree       which pages a mutation touches
      BufferPool      the page cache; the op_dirty quarantine (no-steal)
      Wal             the durability point (a redo-only, fsynced log)
      DiskManager     the database file; deferred metadata

  One mutation (Put or Remove):

      1. pool->BeginOp()           open the quarantine bracket
      2. tree->Put / Remove        mutate pages in the pool; every frame
                                   dirtied is quarantined op_dirty
      3. pool->OpDirtyFrames()     the operation's data-page images, and
         disk->TakeOpImages()      its metadata images (header, free
                                   links) — each drained exactly once
      4. wal->AppendPageImage ×N   buffer all of them, then
         wal->Commit()             one contiguous append + one fsync:
                                   the operation is durable the moment
                                   this returns Ok
      5. pool->EndOp()             close the bracket; the frames become
                                   ordinary dirty pages, free to evict

      A failure in step 2 or 4 skips EndOp and poisons the handle
      instead; see below.

  The checkpoint, which brings the database file up to date:

      1. pool->FlushAllPages()     every committed-dirty page -> db file
      2. disk->Checkpoint()        pending free links + header -> db file
      3. disk->Sync()              fsync: the db file is now current
      4. wal->Reset()              only now may the log forget

      It runs when the log outgrows its threshold, and at Close(). Steps
      1–3 may crash or tear in any order and any state: until step 4
      completes, the log still holds images of everything being written,
      and the next Open replays them over whatever the crash left.

  The invariant chain that makes a crash at any instant safe:

      - An operation is durable if and only if its COMMIT record is in
        the fsynced log (recovery discards trailing runs with no commit).
      - Every image the operation depends on precedes its COMMIT record,
        appended as one contiguous run.
      - Uncommitted bytes never reach the database file: the pool never
        writes op_dirty frames, and metadata leaves the disk manager only
        as logged images or checkpoint writes.
      - The log is reset only once the database file durably holds
        everything the log describes.

  Poisoning is the failure story. A mutation that dies midway has already
  rewritten pages in the pool, and a redo-only log cannot undo them. So
  the handle turns every later call into StatusCode::Closed, discards the
  operation's pending log images, and — deliberately — never calls
  EndOp: the tainted frames stay quarantined so no flush or eviction path
  can ever write them. Close() on a poisoned handle skips the checkpoint
  and leaves the log intact; reopening the database replays exactly the
  operations that committed and nothing else.

  One note on Open's ordering. The exclusive file lock comes first —
  before even recovery, because replaying and truncating the log while
  another handle is appending to it destroys acknowledged writes (see
  AcquireDatabaseLock). Recovery then runs before DiskManager::Open
  parses the header, because replay may rewrite any page — the header
  included. And the fresh-database bootstrap (allocating the root) is
  deliberately not made durable on its own: the header change rides into
  the first logged operation's images, and until then a crash simply
  re-bootstraps an equally empty database.
*/

namespace tinydb {

namespace {
constexpr std::size_t POOL_FRAME_COUNT = 64;

// When the log outgrows this, the committing operation checkpoints: the
// pool and deferred metadata are flushed to the database file, fsynced,
// and the log starts over. Roughly 250 page images.
constexpr std::uint64_t CHECKPOINT_THRESHOLD_BYTES = 1U << 20U;

// The failing errno as an IoError status, tagged with the operation.
auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

// A new file's data can be durable while its directory entry is not; the
// entry is durable only once the directory itself has been fsynced.
auto SyncParentDirectory(const std::filesystem::path &path) -> Status {
  auto parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }

  auto dir_fd = UniqueFd(io::Open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!dir_fd.Valid()) {
    return ErrnoStatus("open directory");
  }
  if (io::Fsync(dir_fd.Get()) < 0) {
    return ErrnoStatus("fsync directory");
  }
  return {};
}

// One live handle per database, enforced with an exclusive flock on the
// database file, held until the handle closes. This must happen before
// recovery runs — not merely before DiskManager::Open — because a second
// process's recovery would replay and truncate the log while the first
// process's Wal still remembers the old tail: the first handle's next
// commit would then land past a hole, and recovery after a crash would
// stop at the hole and drop acknowledged writes. flock is per open file
// description, so a second handle in the same process conflicts too.
//
// Locking may create the file (an empty one; DiskManager::Open writes the
// header later). When it does, the directory entry gets fsynced here:
// recovery may go on to replay committed pages into this file, and it
// only syncs the parent directory for files it created itself — an
// entry-less file with a truncated log would lose everything.
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
}  // namespace

auto StorageEngine::Open(const std::filesystem::path &path) -> Result<StorageEngine> {
  // The lock comes first — even recovery must not touch a database that
  // another handle has open (see AcquireDatabaseLock).
  auto lock = AcquireDatabaseLock(path);
  if (!lock) {
    return std::unexpected(std::move(lock).error());
  }

  // Recovery runs before anything reads the database file: replay may
  // rewrite any page, including the header DiskManager::Open parses.
  const auto wal_path = Wal::PathFor(path);
  if (auto status = Wal::Recover(path, wal_path); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  auto disk_result = DiskManager::Open(path);
  if (!disk_result) {
    return std::unexpected(std::move(disk_result).error());
  }
  auto disk = std::make_unique<DiskManager>(*std::move(disk_result));
  auto pool = std::make_unique<BufferPool>(disk.get(), POOL_FRAME_COUNT);
  auto pages = std::make_unique<BufferPoolPageSource>(pool.get());

  // The log is bound to this database by its UUID: a fresh log is stamped
  // with it, an existing log must already carry it.
  auto wal_result = Wal::Open(wal_path, disk->Uuid());
  if (!wal_result) {
    return std::unexpected(std::move(wal_result).error());
  }
  auto wal = std::make_unique<Wal>(*std::move(wal_result));

  // A fresh database has no root yet: allocate one zeroed page, record its
  // id in the in-memory header, and let the tree bootstrap it as an empty
  // leaf. None of this needs to be durable — the header change rides into
  // the first logged operation's images, and until then a crash just
  // re-bootstraps an equally empty database.
  auto root_page_id = disk->GetRootPageId();
  if (root_page_id == HEADER_PAGE_ID) {
    const auto new_page = pool->NewPage();
    if (!new_page) {
      return std::unexpected(new_page.error());
    }
    root_page_id = new_page->page_id;
    pool->UnpinPage(root_page_id, true);
    disk->SetRootPageId(root_page_id);
  }

  auto tree_result = BPlusTree::Open(pages.get(), root_page_id);
  if (!tree_result) {
    return std::unexpected(std::move(tree_result).error());
  }
  auto tree = std::make_unique<BPlusTree>(*std::move(tree_result));

  return StorageEngine(path, std::move(*lock), std::move(disk), std::move(pool), std::move(pages), std::move(tree),
                       std::move(wal));
}

StorageEngine::StorageEngine(std::filesystem::path path, UniqueFd lock_fd, std::unique_ptr<DiskManager> disk,
                             std::unique_ptr<BufferPool> pool, std::unique_ptr<BufferPoolPageSource> pages,
                             std::unique_ptr<BPlusTree> tree, std::unique_ptr<Wal> wal)
    : path_(std::move(path)),
      lock_fd_(std::move(lock_fd)),
      disk_(std::move(disk)),
      pool_(std::move(pool)),
      pages_(std::move(pages)),
      tree_(std::move(tree)),
      wal_(std::move(wal)) {}

StorageEngine::StorageEngine(StorageEngine &&other) noexcept
    : path_(std::move(other.path_)),
      closed_(other.closed_),
      poisoned_(other.poisoned_),
      lock_fd_(std::move(other.lock_fd_)),
      disk_(std::move(other.disk_)),
      pool_(std::move(other.pool_)),
      pages_(std::move(other.pages_)),
      tree_(std::move(other.tree_)),
      wal_(std::move(other.wal_)) {
  // The moved-from handle keeps no resources; marking it closed makes
  // every later call on it a clean Closed status instead of a null deref.
  other.closed_ = true;
}

StorageEngine::~StorageEngine() { CloseBestEffort(); }

void StorageEngine::CloseBestEffort() noexcept {
  if (const auto status = Close(); !status.Ok()) {
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", path_.c_str(), status.ToString().c_str());
  }
}

auto StorageEngine::operator=(StorageEngine &&other) noexcept -> StorageEngine & {
  if (this != &other) {
    CloseBestEffort();
    // If the close failed, the members are still live: release them in
    // dependency order (the pool's destructor retries its flush while the
    // disk manager is still alive) rather than letting the member-wise
    // assignments below destroy the disk manager out from under the pool.
    tree_.reset();
    pages_.reset();
    pool_.reset();
    disk_.reset();
    wal_.reset();

    path_ = std::move(other.path_);
    closed_ = other.closed_;
    poisoned_ = other.poisoned_;
    // The lock is adopted only after this handle's own teardown above:
    // releasing our old lock any earlier would let another handle open
    // the database while our pool was still flushing into it.
    lock_fd_ = std::move(other.lock_fd_);
    disk_ = std::move(other.disk_);
    pool_ = std::move(other.pool_);
    pages_ = std::move(other.pages_);
    tree_ = std::move(other.tree_);
    wal_ = std::move(other.wal_);
    other.closed_ = true;
  }

  return *this;
}

auto StorageEngine::Put(std::string_view key, std::string_view value) -> Status {
  if (closed_) {
    return Status::Closed("Put on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("Put on a poisoned handle; reopen the database to recover");
  }
  if (key.size() + value.size() > MAX_ENTRY_BYTES) {
    return Status::InvalidArgument("key + value exceeds MAX_ENTRY_BYTES");
  }

  pool_->BeginOp();
  if (auto status = tree_->Put(key, value); !status.Ok()) {
    Poison();
    return status;
  }
  // Root splits are ordinary tree operations now. Persist the new logical
  // root in the same WAL operation as the pages that made it reachable.
  if (tree_->RootPageId() != disk_->GetRootPageId()) {
    disk_->SetRootPageId(tree_->RootPageId());
  }
  return CommitOp();
}

auto StorageEngine::Get(std::string_view key) -> Result<std::optional<std::string>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Get on a closed handle"));
  }
  if (poisoned_) {
    return std::unexpected(Status::Closed("Get on a poisoned handle; reopen the database to recover"));
  }
  return tree_->Get(key);
}

auto StorageEngine::Remove(std::string_view key) -> Status {
  if (closed_) {
    return Status::Closed("Remove on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("Remove on a poisoned handle; reopen the database to recover");
  }

  pool_->BeginOp();
  if (auto status = tree_->Remove(key); !status.Ok()) {
    Poison();
    return status;
  }
  // A collapse promotes the sole child and retires the former root. Logging
  // this metadata beside the page/free-list images makes the new identity
  // crash-atomic with the deletion.
  if (tree_->RootPageId() != disk_->GetRootPageId()) {
    disk_->SetRootPageId(tree_->RootPageId());
  }
  return CommitOp();
}

auto StorageEngine::Scan(std::string_view start,
                         std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Scan on a closed handle"));
  }
  if (poisoned_) {
    return std::unexpected(Status::Closed("Scan on a poisoned handle; reopen the database to recover"));
  }
  return tree_->Scan(start, end);
}

auto StorageEngine::CheckIntegrity() -> Status {
  if (closed_) {
    return Status::Closed("CheckIntegrity on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("CheckIntegrity on a poisoned handle; reopen the database to recover");
  }
  return tree_->CheckIntegrity(disk_->NextPageId(), disk_->FreePages());
}

// Steps 3 through 5 of the mutation protocol in the file comment: collect
// the operation's images, make them durable, release the quarantine —
// then checkpoint if the log has grown past its threshold.
auto StorageEngine::CommitOp() -> Status {
  const auto frames = pool_->OpDirtyFrames();
  const auto disk_images = disk_->TakeOpImages();

  // Nothing changed (say, removing an absent key): nothing to make
  // durable, and Wal::Commit treats an empty commit as a bug.
  if (frames.empty() && disk_images.empty()) {
    pool_->EndOp();
    return {};
  }

  for (const auto &[page_id, data] : frames) {
    wal_->AppendPageImage(page_id, data);
  }
  for (const auto &image : disk_images) {
    wal_->AppendPageImage(image.page_id, image.data.data());
  }
  if (auto status = wal_->Commit(); !status.Ok()) {
    Poison();
    return status;
  }
  // The images are durable: the frames go back to being ordinary dirty
  // pages, safe to evict.
  pool_->EndOp();

  if (wal_->SizeBytes() > CHECKPOINT_THRESHOLD_BYTES) {
    // The operation itself already committed; a checkpoint failure poisons
    // the handle but loses nothing — the log still holds every image.
    if (auto status = Checkpoint(); !status.Ok()) {
      Poison();
      return status;
    }
  }
  return {};
}

// The checkpoint protocol from the file comment. The ordering is the
// entire point: the log may only forget (Reset) once everything it
// describes is durably in the database file — flushed, checkpointed, and
// fsynced. The first three steps are free to fail or tear; recovery
// replays them from the still-intact log.
auto StorageEngine::Checkpoint() -> Status {
  if (auto status = pool_->FlushAllPages(); !status.Ok()) {
    return status;
  }
  if (auto status = disk_->Checkpoint(); !status.Ok()) {
    return status;
  }
  if (auto status = disk_->Sync(); !status.Ok()) {
    return status;
  }
  // Only now, with the database file caught up and durable, may the log
  // forget: truncating any earlier is data loss on the next crash.
  return wal_->Reset();
}

// See "Poisoning" in the file comment. The two calls this makes — and the
// one it pointedly does not — are the whole mechanism: mark the handle,
// drop the dead operation's buffered log images, and never call EndOp, so
// the pool keeps the half-written frames quarantined forever.
void StorageEngine::Poison() {
  poisoned_ = true;
  wal_->DiscardPending();
}

auto StorageEngine::Close() -> Status {
  if (!disk_) {  // fully closed already, or moved from
    return {};
  }
  closed_ = true;

  // A poisoned engine's in-memory state is not trustworthy, so none of it
  // is flushed as a checkpoint; the log already holds every committed
  // operation and the next Open() replays it. (The pool's destructor still
  // best-effort-writes committed-dirty frames, which is harmless: their
  // images stay in the log because the reset below never runs.)
  if (poisoned_) {
    tree_.reset();
    pages_.reset();
    pool_.reset();
    disk_.reset();
    wal_.reset();
    lock_fd_ = UniqueFd{};  // the next opener recovers from the log
    return {};
  }

  // Release resources only after the checkpoint succeeds: if any step
  // fails, everything stays live — the database lock included — and a
  // second Close() retries, rewriting only what has not been written.
  if (auto status = Checkpoint(); !status.Ok()) {
    return status;
  }
  tree_.reset();
  pages_.reset();
  pool_.reset();
  disk_.reset();
  wal_.reset();
  lock_fd_ = UniqueFd{};
  return {};
}
}  // namespace tinydb
