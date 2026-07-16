#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace tinydb {

namespace txn {
struct DatabaseState;
}

/*
** SEGMENTED REDO WRITE-AHEAD LOG
**
** AppendPageImage collects final data-page images in memory. Commit adds the
** resulting logical database state, encodes the complete transaction, appends
** it once, and synchronizes once; that synchronization is the mutation
** durability point. A transaction never crosses a segment: soft rotation
** archives the current active file, synchronizes its directory entry, then
** creates and synchronizes the next active header before append.
**
** PathFor names the active segment. Immutable archives are sibling files named
** by SegmentPathFor. Reset may delete them only after the caller has written
** and synchronized the same state into the database file. Recover is the only
** path that interprets a non-empty segment sequence; normal Open accepts the
** clean active header left behind by recovery or checkpoint.
*/
class Wal {
 public:
  static constexpr std::uint64_t DEFAULT_SEGMENT_BYTES = 768U << 10U;

  struct CommitInfo {
    std::uint64_t transaction_id;
    std::uint64_t commit_lsn;
  };

  // The companion path is deterministic so recovery can run before the
  // database file is parsed or a normal Wal object exists.
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;
  static auto SegmentPathFor(const std::filesystem::path &wal_path, std::uint64_t segment_id) -> std::filesystem::path;

  // Replays complete committed runs and ignores only an incomplete trailing
  // run. Successful recovery leaves the database durable before truncating
  // the WAL back to its header.
  static auto Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status;

  // Creates or validates a clean header-only WAL bound to database_uuid.
  static auto Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
                   std::uint64_t next_transaction_id = 1, std::uint64_t starting_lsn = 0,
                   std::uint64_t max_segment_bytes = DEFAULT_SEGMENT_BYTES) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;

  Wal(Wal &&) noexcept = default;
  auto operator=(Wal &&) noexcept -> Wal & = default;
  ~Wal() = default;

  void AppendPageImage(page_id_t page_id, const char *data);

  // Used when a mutation fails before commit. This drops only RAM bytes; it
  // never removes previously committed records from the durable log.
  void DiscardPending();
  auto NextCommitLsn(std::size_t page_count) const -> Result<std::uint64_t>;
  auto Commit(txn::DatabaseState state) -> Result<CommitInfo>;
  auto SizeBytes() const -> std::uint64_t;
  auto Reset() -> Status;

 private:
  struct PendingPageImage {
    page_id_t page_id;
    std::array<char, PAGE_SIZE> bytes;
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
        max_segment_bytes_(max_segment_bytes) {}

  auto RotateSegment() -> Status;

  UniqueFd fd_;
  std::filesystem::path wal_path_;
  DatabaseUuid database_uuid_{};
  std::uint64_t segment_id_{1};

  // Durable append offset, excluding pending_ until fsync succeeds.
  std::uint64_t size_bytes_{0};
  std::uint64_t retained_size_bytes_{0};

  // Transaction IDs and LSNs continue across checkpoints. Reset writes a new
  // header whose starting LSN is the first record not yet assigned.
  std::uint64_t next_transaction_id_{1};
  std::uint64_t next_lsn_{1};
  std::uint64_t max_segment_bytes_{DEFAULT_SEGMENT_BYTES};
  bool needs_recovery_{false};
  std::vector<std::filesystem::path> archived_segments_;

  // Final page bytes are retained until Commit can encode PAGE_IMAGE,
  // DATABASE_STATE, and COMMIT as one self-binding transaction.
  std::vector<PendingPageImage> pending_;
};
}  // namespace tinydb
