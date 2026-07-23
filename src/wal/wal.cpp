#include "wal/wal.h"
#include "util/check.h"

#include "io/file_io.h"
#include "io/syscalls.h"
#include "txn/database_state.h"
#include "wal/wal_codec.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

/*
** CURRENT WAL PROTOCOL
**
** The writer collects final data pages in memory, adds the resulting logical
** database state, and appends the complete run with its binding Commit record
** before calling fsync. That fsync is the durability point. Database may
** publish only after it succeeds. A failed append that can be truncated back
** to the known-good tail is a definite abort. Failed repair or ambiguous sync
** prevents more work until recovery establishes the durable tail.
**
** WAL files are append-only durability artifacts.  The writer does not parse
** or replay existing records; the recovery subsystem owns all interpretation
** of persistent WAL input before normal Wal::Open accepts a clean active
** header.
*/

auto Bytes(const std::vector<char> &value) -> std::span<const std::byte> { return std::as_bytes(std::span{value}); }

using io::ErrnoStatus;
using io::FullPread;
using io::FullPwrite;
using io::SyncParentDirectory;

}  // namespace

auto Wal::PathFor(const std::filesystem::path &db_path) -> std::filesystem::path {
  auto wal_path = db_path;
  wal_path += "-wal";
  return wal_path;
}

auto Wal::SegmentPathFor(const std::filesystem::path &wal_path, std::uint64_t segment_id) -> std::filesystem::path {
  auto path = wal_path;
  path += "." + std::to_string(segment_id) + ".segment";
  return path;
}

Wal::Wal(Wal &&other) noexcept
    : fd_(std::move(other.fd_)),
      wal_path_(std::move(other.wal_path_)),
      database_uuid_(other.database_uuid_),
      segment_id_(other.segment_id_),
      size_bytes_(other.size_bytes_),
      next_transaction_id_(other.next_transaction_id_),
      next_lsn_(other.next_lsn_),
      max_segment_bytes_(other.max_segment_bytes_),
      checkpoint_lsn_(other.checkpoint_lsn_),
      needs_recovery_(other.needs_recovery_),
      cleanup_directory_dirty_(other.cleanup_directory_dirty_),
      archived_segments_(std::move(other.archived_segments_)) {}

auto Wal::Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
               std::uint64_t next_transaction_id, std::uint64_t starting_lsn,
               std::uint64_t max_segment_bytes) -> Result<Wal> {
  if (next_transaction_id == 0 || max_segment_bytes <= wal_format::HEADER_BYTES) {
    return std::unexpected(Status::InvalidArgument("invalid WAL transaction frontier"));
  }
  const auto requested_starting_lsn = starting_lsn;
  if (starting_lsn == 0) {
    starting_lsn = 1;
  }
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  Wal wal(std::move(fd), wal_path, database_uuid, 1, static_cast<std::uint64_t>(file_stat.st_size), next_transaction_id,
          starting_lsn, max_segment_bytes);

  if (file_stat.st_size == 0) {
    // Fresh WAL durability ordering is contents -> file fsync -> directory
    // fsync. A crash before completion leaves the short-header case Recover
    // handles without pretending any transaction committed.
    const auto encoded =
        wal_format::EncodeHeader(wal_format::Header{.database_uuid = database_uuid, .starting_lsn = starting_lsn});
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    if (auto status = FullPwrite(wal.fd_.Get(), encoded->data(), encoded->size(), 0); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    if (io::Fsync(wal.fd_.Get()) < 0) {
      return std::unexpected(ErrnoStatus("fsync"));
    }
    if (auto status = SyncParentDirectory(wal_path); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    wal.size_bytes_ = wal_format::HEADER_BYTES;
    return wal;
  }

  if (file_stat.st_size != static_cast<off_t>(wal_format::HEADER_BYTES)) {
    // Normal Open accepts only a clean header-only log. Any records must first
    // pass through Recover, which is the sole parser allowed to replay/truncate.
    auto prefix = std::array<std::byte, wal_format::MAGIC.size()>{};
    const auto prefix_size = std::min<std::size_t>(prefix.size(), static_cast<std::size_t>(file_stat.st_size));
    const auto read = FullPread(wal.fd_.Get(), prefix.data(), prefix_size, 0);
    if (!read) {
      return std::unexpected(read.error());
    }
    if (prefix_size < wal_format::MAGIC.size() || !std::ranges::equal(prefix, wal_format::MAGIC)) {
      return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB WAL format"));
    }
    return std::unexpected(Status::Corruption("WAL was not recovered before open"));
  }
  auto encoded = std::vector<char>(wal_format::HEADER_BYTES);
  const auto read = FullPread(wal.fd_.Get(), encoded.data(), encoded.size(), 0);
  if (!read) {
    return std::unexpected(read.error());
  }
  const auto header = wal_format::DecodeHeader(Bytes(encoded));
  if (!header) {
    return std::unexpected(header.error());
  }
  if (header->database_uuid != database_uuid) {
    return std::unexpected(
        Status::InvalidArgument("write-ahead log does not belong to this database: " + wal_path.string()));
  }
  if (requested_starting_lsn != 0 && header->starting_lsn != requested_starting_lsn) {
    return std::unexpected(Status::Corruption("WAL starting LSN disagrees with the database checkpoint"));
  }
  wal.segment_id_ = header->segment_id;
  wal.next_lsn_ = header->starting_lsn;
  return wal;
}

auto Wal::RotateSegment() -> Status {
  TINYDB_CHECK(size_bytes_ > wal_format::HEADER_BYTES, "rotating an empty WAL segment");
  const auto archive_path = SegmentPathFor(wal_path_, segment_id_);
  auto exists_error = std::error_code{};
  if (std::filesystem::exists(archive_path, exists_error) || exists_error) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("next WAL archive path is not safely available");
  }
  if (io::Rename(wal_path_, archive_path) < 0) {
    return ErrnoStatus("archive WAL segment");
  }
  // A failed directory sync makes the active/archive name transition
  // indeterminate across power loss. The old fd remains valid, but appending
  // through it would no longer have one discoverable active identity.
  if (auto status = SyncParentDirectory(wal_path_); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("WAL segment archive synchronization failed");
  }

  auto next_fd = UniqueFd(io::Open(wal_path_, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
  if (!next_fd.Valid()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not create the next WAL segment");
  }
  const auto next_segment_id = segment_id_ + 1U;
  if (next_segment_id == 0) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("WAL segment ID space exhausted after archive");
  }
  const auto header = wal_format::EncodeHeader(wal_format::Header{
      .database_uuid = database_uuid_,
      .segment_id = next_segment_id,
      .starting_lsn = next_lsn_,
  });
  if (!header) {
    needs_recovery_ = true;
    return Status::NeedsRecovery(header.error().Message());
  }
  if (auto status = FullPwrite(next_fd.Get(), header->data(), header->size(), 0); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not write the next WAL segment header");
  }
  if (io::Fsync(next_fd.Get()) < 0) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not synchronize the next WAL segment header");
  }
  if (auto status = SyncParentDirectory(wal_path_); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("next WAL segment directory entry is indeterminate");
  }

  archived_segments_.push_back(ArchivedSegment{
      .path = archive_path,
      .final_lsn = next_lsn_ - 1U,
      .size_bytes = size_bytes_,
  });
  fd_ = std::move(next_fd);
  segment_id_ = next_segment_id;
  size_bytes_ = wal_format::HEADER_BYTES;
  return {};
}

auto Wal::NextCommitLsn(std::size_t page_count) const -> Result<std::uint64_t> {
  auto lock = std::lock_guard(mutex_);
  if (page_count == 0 || page_count > std::numeric_limits<std::uint32_t>::max() - 2U ||
      next_lsn_ >= std::numeric_limits<std::uint64_t>::max() ||
      page_count > std::numeric_limits<std::uint64_t>::max() - next_lsn_ - 1U) {
    return std::unexpected(Status::ResourceExhausted("WAL LSN space exhausted"));
  }
  return next_lsn_ + page_count + 1U;
}

auto Wal::Prepare(std::span<const wal_format::PageImageView> pages,
                  txn::DatabaseState state) const -> Result<wal_format::EncodedTransaction> {
  auto lock = std::lock_guard(mutex_);
  TINYDB_CHECK(fd_.Valid(), "preparing through a moved-from log");
  if (needs_recovery_) {
    return std::unexpected(Status::NeedsRecovery("WAL tail is not trustworthy; reopen the database"));
  }
  if (state.transaction_id != 0 && state.transaction_id != next_transaction_id_) {
    return std::unexpected(Status::InvalidArgument("database state has the wrong WAL transaction ID"));
  }
  return wal_format::EncodeTransaction(next_transaction_id_, next_lsn_, pages, state);
}

auto Wal::Commit(wal_format::EncodedTransaction transaction) -> Result<CommitInfo> {
  auto lock = std::lock_guard(mutex_);
  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  if (needs_recovery_) {
    return std::unexpected(Status::NeedsRecovery("WAL tail is not trustworthy; reopen the database"));
  }
  // A prepared transaction can cross no other writer. Recheck its frontier
  // before the first byte is appended so stale or foreign preparation remains
  // a definite failure.
  if (transaction.transaction_id != next_transaction_id_ || transaction.first_lsn != next_lsn_) {
    return std::unexpected(Status::InvalidArgument("prepared WAL transaction has a stale identity"));
  }

  // Rotation is soft: a transaction larger than the configured target owns an
  // oversized empty segment rather than being split across files.
  if (size_bytes_ > wal_format::HEADER_BYTES && size_bytes_ + transaction.bytes.size() > max_segment_bytes_) {
    if (auto status = RotateSegment(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }

  /*
  ** DURABILITY POINT
  **
  ** One contiguous append preserves record order. The subsequent fsync is the
  ** exact point after which Database may acknowledge durability. Nothing
  ** below advances the known-good tail until that synchronization succeeds.
  */
  if (auto status = FullPwrite(fd_.Get(), transaction.bytes.data(), transaction.bytes.size(), size_bytes_);
      !status.Ok()) {
    /*
    ** A failed append cannot contain the complete final COMMIT record: the
    ** write loop stops at the first failed/short syscall and COMMIT is last.
    ** Truncating to known_good_size therefore converts the result into a
    ** definite abort. If that repair fails, live appends are forbidden because
    ** their offset could splice into an unknown tail.
    */
    if (io::Ftruncate(fd_.Get(), size_bytes_) < 0) {
      needs_recovery_ = true;
      return std::unexpected(Status::NeedsRecovery("WAL append and known-good tail repair both failed"));
    }
    return std::unexpected(std::move(status));
  }
  if (io::Fsync(fd_.Get()) < 0) {
    // The kernel may have persisted the COMMIT despite reporting failure. No
    // live repair can turn that into a definite outcome without another
    // ambiguous durability boundary, so recovery must decide on reopen.
    needs_recovery_ = true;
    return std::unexpected(Status::IndeterminateCommit(ErrnoStatus("fsync").Message()));
  }
  size_bytes_ += transaction.bytes.size();
  next_lsn_ = transaction.next_lsn;
  const auto result = CommitInfo{.transaction_id = transaction.transaction_id, .commit_lsn = transaction.commit_lsn};
  ++next_transaction_id_;
  return result;
}

auto Wal::SizeBytes() const -> std::uint64_t {
  auto lock = std::lock_guard(mutex_);
  auto live_bytes = std::uint64_t{0};
  for (const auto &segment : archived_segments_) {
    if (segment.final_lsn > checkpoint_lsn_) {
      live_bytes += segment.size_bytes;
    }
  }
  // A covered active segment remains the append target. Keeping it avoids a
  // destructive truncate during ordinary maintenance; it contributes only a
  // logical header to checkpoint pressure until a new transaction is appended.
  live_bytes += next_lsn_ - 1U > checkpoint_lsn_ ? size_bytes_ : wal_format::HEADER_BYTES;
  return live_bytes;
}

auto Wal::SegmentCount() const -> std::size_t {
  auto lock = std::lock_guard(mutex_);
  // The active append target exists for the lifetime of Wal. Archives remain
  // observable until a checkpoint has durably covered and removed them.
  return archived_segments_.size() + 1U;
}

auto Wal::CleanupCheckpointed(std::uint64_t checkpoint_lsn) -> Status {
  auto lock = std::lock_guard(mutex_);
  TINYDB_CHECK(fd_.Valid(), "cleaning a moved-from log");
  TINYDB_CHECK(checkpoint_lsn >= checkpoint_lsn_, "WAL checkpoint frontier moved backward");
  checkpoint_lsn_ = checkpoint_lsn;

  /*
  ** COVERED SEGMENT CLEANUP
  **
  ** Rotation is the only operation that makes a segment immutable. Therefore
  ** cleanup never truncates or renames the active append target. Archives are
  ** ordered, so deletion stops at the first segment containing a record newer
  ** than the durable superblock. A failed unlink leaves all remaining files as
  ** redundant, checkpoint-covered history.
  */
  while (!archived_segments_.empty() && archived_segments_.front().final_lsn <= checkpoint_lsn) {
    const auto segment = archived_segments_.front();
    if (io::Unlink(segment.path) < 0 && errno != ENOENT) {
      return io::ErrnoStatus("remove checkpointed WAL segment");
    }
    archived_segments_.pop_front();
    cleanup_directory_dirty_ = true;
  }
  if (cleanup_directory_dirty_) {
    if (auto status = SyncParentDirectory(wal_path_); !status.Ok()) {
      return status;
    }
    cleanup_directory_dirty_ = false;
  }
  return {};
}

}  // namespace tinydb
