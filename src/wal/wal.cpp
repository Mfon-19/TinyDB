#include "wal/wal.h"

#include "io/file_io.h"
#include "io/syscalls.h"
#include "util/check.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

auto Bytes(const std::vector<char> &value) -> std::span<const std::byte> { return std::as_bytes(std::span{value}); }

using io::ErrnoStatus;
using io::FullPread;
using io::FullPwrite;

// Encodes and writes a clean WAL header at offset zero. It synchronizes the
// file but does not truncate it or synchronize the parent directory.
auto WriteCleanHeader(int fd, const DatabaseUuid &database_uuid, std::uint64_t starting_lsn) -> Status {
  const auto encoded =
      wal_format::EncodeHeader(wal_format::Header{.database_uuid = database_uuid, .starting_lsn = starting_lsn});
  if (!encoded) {
    return encoded.error();
  }
  if (auto status = FullPwrite(fd, encoded->data(), encoded->size(), 0); !status.Ok()) {
    return status;
  }
  if (io::Fsync(fd) < 0) {
    return ErrnoStatus("fsync WAL header");
  }
  return {};
}

}  // namespace

auto Wal::PathFor(const std::filesystem::path &db_path) -> std::filesystem::path {
  auto wal_path = db_path;
  wal_path += "-wal";
  return wal_path;
}

Wal::Wal(Wal &&other) noexcept
    : fd_(std::move(other.fd_)),
      database_uuid_(other.database_uuid_),
      size_bytes_(other.size_bytes_),
      next_lsn_(other.next_lsn_),
      needs_recovery_(other.needs_recovery_) {}

// Opens or creates a header-only WAL for the specified database and starting
// LSN. An empty file receives a new durable header. A nonempty file must
// contain only a supported header with the specified database UUID and starting
// LSN.
auto Wal::Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
               std::uint64_t starting_lsn) -> Result<Wal> {
  if (starting_lsn == 0) {
    return std::unexpected(Status::InvalidArgument("WAL starting LSN must be nonzero"));
  }
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open WAL"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat WAL"));
  }
  auto wal = Wal(std::move(fd), database_uuid, static_cast<std::uint64_t>(file_stat.st_size), starting_lsn);
  if (file_stat.st_size == 0) {
    if (auto status = WriteCleanHeader(wal.fd_.Get(), database_uuid, starting_lsn); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    if (auto status = io::SyncParentDirectory(wal_path); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    wal.size_bytes_ = wal_format::HEADER_BYTES;
    return wal;
  }
  if (file_stat.st_size != static_cast<off_t>(wal_format::HEADER_BYTES)) {
    auto prefix = std::array<std::byte, wal_format::MAGIC.size()>{};
    const auto prefix_size = std::min(prefix.size(), static_cast<std::size_t>(file_stat.st_size));
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
  if (header->starting_lsn != starting_lsn) {
    return std::unexpected(Status::Corruption("WAL starting LSN disagrees with the database checkpoint"));
  }
  wal.next_lsn_ = header->starting_lsn;
  return wal;
}

// Returns the LSN that Prepare and Commit expect next. It does not consume the
// LSN. It reports resource exhaustion before the LSN space ends.
auto Wal::NextCommitLsn() const -> Result<std::uint64_t> {
  auto lock = std::lock_guard(mutex_);
  if (next_lsn_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("WAL LSN space exhausted"));
  }
  return next_lsn_;
}

// Encodes one transaction with the current next_lsn_. It performs no file I/O.
// The function rejects a WAL that needs recovery.
auto Wal::Prepare(std::span<const wal_format::PageImageView> pages,
                  txn::DatabaseState state) const -> Result<wal_format::EncodedTransaction> {
  auto lock = std::lock_guard(mutex_);
  TINYDB_CHECK(fd_.Valid(), "preparing through a moved-from log");
  if (needs_recovery_) {
    return std::unexpected(Status::NeedsRecovery("WAL tail is not trustworthy; reopen the database"));
  }
  return wal_format::EncodeTransaction(next_lsn_, pages, state);
}

// Appends one prepared transaction at the known-good WAL tail and synchronizes
// the file. Success advances the tracked file size and next LSN. An append error
// triggers truncation to the old tail. A truncation or synchronization error
// makes the handle require recovery.
auto Wal::Commit(wal_format::EncodedTransaction transaction) -> Result<std::uint64_t> {
  auto lock = std::lock_guard(mutex_);
  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  if (needs_recovery_) {
    return std::unexpected(Status::NeedsRecovery("WAL tail is not trustworthy; reopen the database"));
  }
  if (transaction.commit_lsn != next_lsn_ || next_lsn_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::InvalidArgument("prepared WAL transaction has a stale identity"));
  }

  if (auto status = FullPwrite(fd_.Get(), transaction.bytes.data(), transaction.bytes.size(), size_bytes_);
      !status.Ok()) {
    if (io::Ftruncate(fd_.Get(), size_bytes_) < 0) {
      needs_recovery_ = true;
      return std::unexpected(Status::NeedsRecovery("WAL append and known-good tail repair both failed"));
    }
    return std::unexpected(std::move(status));
  }
  if (io::Fsync(fd_.Get()) < 0) {
    needs_recovery_ = true;
    return std::unexpected(Status::IndeterminateCommit(ErrnoStatus("fsync WAL").Message()));
  }
  size_bytes_ += transaction.bytes.size();
  ++next_lsn_;
  return transaction.commit_lsn;
}

// Returns the tracked WAL byte count under the mutex. It does not read file
// metadata from the filesystem.
auto Wal::SizeBytes() const -> std::uint64_t {
  auto lock = std::lock_guard(mutex_);
  return size_bytes_;
}

// Replaces the fully checkpointed WAL with a clean header that starts at
// next_lsn_. The checkpoint LSN must cover the complete durable tail. A
// header-only WAL needs no change. An I/O error makes the handle require
// recovery.
auto Wal::Reset(std::uint64_t checkpoint_lsn) -> Status {
  auto lock = std::lock_guard(mutex_);
  TINYDB_CHECK(fd_.Valid(), "resetting a moved-from log");
  if (needs_recovery_) {
    return Status::NeedsRecovery("WAL tail is not trustworthy; reopen the database");
  }
  if (checkpoint_lsn == std::numeric_limits<std::uint64_t>::max() || checkpoint_lsn + 1U != next_lsn_) {
    return Status::InvalidArgument("checkpoint LSN does not cover the complete durable WAL tail");
  }
  if (size_bytes_ == wal_format::HEADER_BYTES) {
    return {};
  }

  // The database superblock already covers this history. From this point an
  // empty, partial, or complete replacement header is recoverable without any
  // transaction bytes.
  if (io::Ftruncate(fd_.Get(), 0) < 0) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not truncate checkpointed WAL");
  }
  if (auto status = WriteCleanHeader(fd_.Get(), database_uuid_, next_lsn_); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery(status.Message());
  }
  size_bytes_ = wal_format::HEADER_BYTES;
  return {};
}

}  // namespace tinydb
