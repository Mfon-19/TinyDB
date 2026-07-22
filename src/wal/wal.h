#pragma once

#include <tinydb/status.h>
#include "io/unique_fd.h"
#include "storage/database_uuid.h"
#include "wal/wal_codec.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace tinydb {

/*
** SEGMENTED REDO WRITE-AHEAD LOG
**
** Prepare encodes final data-page images and their logical database state
** without touching the filesystem. Commit appends that owned byte sequence
** once and synchronizes once; that synchronization is the mutation durability
** point. A transaction never crosses a segment: soft rotation archives the
** current active file, synchronizes its directory entry, then creates and
** synchronizes the next active header before append.
**
** PathFor names the active segment. Immutable archives are sibling files named
** by SegmentPathFor. CleanupCheckpointed may delete only archives wholly
** covered by a durable superblock; the active append target is never rewritten
** by ordinary checkpointing. The recovery subsystem is the only parser of
** non-empty segment sequences before normal Open.
*/
class Wal {
 public:
  struct CommitInfo {
    std::uint64_t transaction_id;
    std::uint64_t commit_lsn;
  };

  // The companion path is deterministic so recovery can run before the
  // database file is parsed or a normal Wal object exists.
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;
  static auto SegmentPathFor(const std::filesystem::path &wal_path, std::uint64_t segment_id) -> std::filesystem::path;

  // Creates or validates a clean header-only WAL bound to database_uuid.
  static auto Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
                   std::uint64_t next_transaction_id, std::uint64_t starting_lsn,
                   std::uint64_t max_segment_bytes) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;

  Wal(Wal &&) noexcept = default;
  auto operator=(Wal &&) -> Wal & = delete;
  ~Wal() = default;

  auto NextCommitLsn(std::size_t page_count) const -> Result<std::uint64_t>;
  auto Prepare(std::span<const wal_format::PageImageView> pages,
               txn::DatabaseState state) const -> Result<wal_format::EncodedTransaction>;
  auto Commit(wal_format::EncodedTransaction transaction) -> Result<CommitInfo>;

  // Bytes still contributing to checkpoint pressure. A covered active segment
  // remains physically present but counts only as its logical header.
  auto SizeBytes() const -> std::uint64_t;

  // Counts the active append target plus immutable archives not yet removed.
  auto SegmentCount() const -> std::size_t;

  // Required state: a durable superblock covers checkpoint_lsn. Removes only
  // immutable segments whose final record is at or before that frontier.
  // Failure may leave redundant history but never removes live recovery data.
  auto CleanupCheckpointed(std::uint64_t checkpoint_lsn) -> Status;

 private:
  struct ArchivedSegment {
    std::filesystem::path path;
    std::uint64_t final_lsn{0};
    std::uint64_t size_bytes{0};
  };

  Wal(UniqueFd fd, std::filesystem::path wal_path, DatabaseUuid database_uuid, std::uint64_t segment_id,
      std::uint64_t size_bytes, std::uint64_t next_transaction_id, std::uint64_t next_lsn,
      std::uint64_t max_segment_bytes)
      : fd_(std::move(fd)),
        wal_path_(std::move(wal_path)),
        database_uuid_(database_uuid),
        segment_id_(segment_id),
        size_bytes_(size_bytes),
        next_transaction_id_(next_transaction_id),
        next_lsn_(next_lsn),
        max_segment_bytes_(max_segment_bytes),
        checkpoint_lsn_(next_lsn - 1U),
        mutex_(std::make_unique<std::mutex>()) {}

  auto RotateSegment() -> Status;

  UniqueFd fd_;
  std::filesystem::path wal_path_;
  DatabaseUuid database_uuid_{};
  std::uint64_t segment_id_{1};

  // Durable append offset. It advances only after fsync succeeds.
  std::uint64_t size_bytes_{0};

  // Transaction IDs and LSNs continue across checkpoints. checkpoint_lsn_
  // removes covered history from pressure accounting without rewriting the
  // active append target.
  std::uint64_t next_transaction_id_{1};
  std::uint64_t next_lsn_{1};
  std::uint64_t max_segment_bytes_;
  std::uint64_t checkpoint_lsn_{0};
  bool needs_recovery_{false};
  bool cleanup_directory_dirty_{false};
  std::deque<ArchivedSegment> archived_segments_;
  std::unique_ptr<std::mutex> mutex_;
};
}  // namespace tinydb
