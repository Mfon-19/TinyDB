#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace tinydb {

// Important properties of this storage engine informing the WAL design:
// single-threaded, single-operation.
// The typical WAL is steal + no-force, meaning the database can evict a dirty
// page from the buffer pool and write it out to disk before a transaction is
// committed, and it does not need to write out changes to disk before a
// transaction commits.
// The log format is typically physiological, record the page + slot number changed
// + the actual change.
//
// We implement no-force here like a typical WAL, but do no-steal (cannot evict and
// write back a page before commit) since this WAL is redo-only. We don't need the
// full complexity of ARIES since we won't run into a situation where concurrent
// or very large writes exceed the number of frames available in our buffer pool,
// forcing a write back of dirty frames for uncommitted transactions. No-steal ensures
// that we don't run into a situation where we need to undo a write in our database
// file of an uncommitted transaction. (Enforcing no-steal is the buffer pool's job,
// not this class's: a frame dirtied by the in-flight operation is never evicted.)
//
// We are also using the entire page image instead of physiological logging to sidestep
// the ARIES-style pageLSN, also the page is sitting in memory. Cheap.
// So every log record is crc-framed: [u32 length | u32 crc32 | payload]
// Payload is: PageImage{page_id, 4096-byte post-image} or Commit
//
// wal_header: {u32 magic}, written once at creation like the database header;
// the magic doubles as the format version (a format change mints a new magic)
// record: {u32 payload_length | u32 crc32 | payload}
// payload: PAGE_IMAGE {u8 type=1, u64 page_id, char data[4096]}
//        | COMMIT {u8 type=2}
// One committed op on disk = PAGE_IMAGE* COMMIT, written as a single buffered write
// + one fsync, so a crash leaves either nothing or a torn run that the CRC/no-commit
// scan discards.
//
// Lifecycle: Recover() first (replays the log into the database file and empties
// it), then open the database, then Open() the log for appending.
//
// I/O failures are reported as Status / Result values; nothing here throws.
class Wal {
 public:
  // The log's on-disk name for the database at db_path ("data.db-wal").
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;

  // Replays every committed operation in the log at wal_path into the
  // database file at db_path, fsyncs it, and truncates the log to empty.
  // A torn tail — a record with a bad length or CRC, or a trailing run of
  // page images with no commit record — is discarded, not an error. A no-op
  // when the log is missing or empty. Idempotent: a crash during recovery
  // just means recovery runs again on the next open. Must run before the
  // database file is parsed (DiskManager::Open), because replay may rewrite
  // any page, including the header.
  static auto Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status;

  // Opens or creates the log at wal_path for appending. Expects an empty or
  // fully committed log; run Recover() first after any unclean shutdown.
  static auto Open(const std::filesystem::path &wal_path) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;

  // UniqueFd carries the descriptor across moves and closes it on
  // destruction, so the compiler-generated special members are correct.
  Wal(Wal &&) noexcept = default;
  auto operator=(Wal &&) noexcept -> Wal & = default;
  ~Wal() = default;

  // Buffers the post-image of one page mutated by the in-flight operation.
  // Nothing reaches disk until Commit().
  void AppendPageImage(page_id_t page_id, const char *data);

  // Drops buffered images for an operation that failed before Commit().
  void DiscardPending();

  // Writes the buffered images plus a commit record and fsyncs the log:
  // the operation's durability point. Committing with nothing buffered is a
  // bug and aborts — the caller skips logging for operations that changed
  // nothing. On failure the log tail may be torn; the caller must stop
  // accepting work, and the next Recover() discards the tail.
  auto Commit() -> Status;

  // Bytes currently in the log — the caller's checkpoint trigger.
  auto SizeBytes() const -> std::uint64_t;

  // Truncates the log to empty after a checkpoint. Only call once every
  // change the log describes has been written to the database file AND
  // fsynced there; truncating earlier is data loss on the next crash.
  auto Reset() -> Status;

 private:
  Wal(UniqueFd fd, std::uint64_t size_bytes) : fd_(std::move(fd)), size_bytes_(size_bytes) {}

  // The on-disk log file. An invalid fd means this object was moved from.
  UniqueFd fd_;
  std::uint64_t size_bytes_{0};

  // Encoded records for the in-flight operation, flushed by Commit().
  std::vector<char> pending_;
};
}  // namespace tinydb
