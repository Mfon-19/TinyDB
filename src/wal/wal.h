#pragma once

#include <tinydb/status.h>
#include "io/unique_fd.h"
#include "storage/database_uuid.h"
#include "wal/wal_codec.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <utility>

namespace tinydb {

/*
** WAL FILE LIFECYCLE
**
** Wal manages one active redo-log file. TinyDB does not create WAL segments or
** archive files.
**
** The WAL always uses buffered I/O. PageIoMode selects the transport for the
** database page file, but it does not change the WAL format or write path.
** Direct I/O is useful for fixed, page-aligned random access; WAL transactions
** are variable-length appends followed by one fsync and may require truncating
** a failed tail. Keeping that path buffered avoids alignment staging without
** weakening its explicit durability boundary.
**
** Prepare encodes one transaction in memory with the caller's commit LSN,
** the same LSN the caller obtained from NextCommitLsn and sealed into its
** page images. It performs no file I/O.
**
** Commit appends the prepared transaction and calls fsync once; it reports
** success only after that synchronization establishes the transaction's
** durability point.
**
** Reset runs only after a checkpoint writes all committed WAL changes to the
** database file. The checkpoint records that state in a durable superblock.
** Recovery no longer needs the WAL transactions from before that checkpoint.
** Reset then truncates the WAL and writes a clean header that starts at the
** first LSN after the checkpoint, synchronizing the replacement header before
** it reports success.
*/
class Wal {
 public:
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;
  static auto Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
                   std::uint64_t starting_lsn) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;
  Wal(Wal &&other) noexcept;
  auto operator=(Wal &&) -> Wal & = delete;
  ~Wal() = default;

  auto NextCommitLsn() const -> Result<std::uint64_t>;
  auto Prepare(std::uint64_t commit_lsn, std::span<const wal_format::PageImageView> pages,
               txn::DatabaseState state) const -> Result<wal_format::EncodedTransaction>;
  auto Commit(wal_format::EncodedTransaction transaction) -> Result<std::uint64_t>;

  auto SizeBytes() const -> std::uint64_t;

  auto Reset(std::uint64_t checkpoint_lsn) -> Status;

 private:
  Wal(UniqueFd fd, DatabaseUuid database_uuid, std::uint64_t size_bytes, std::uint64_t next_lsn)
      : fd_(std::move(fd)), database_uuid_(database_uuid), size_bytes_(size_bytes), next_lsn_(next_lsn) {}

  UniqueFd fd_;
  DatabaseUuid database_uuid_{};
  std::uint64_t size_bytes_{0};
  std::uint64_t next_lsn_{1};
  bool needs_recovery_{false};
  mutable std::mutex mutex_;
};

}  // namespace tinydb
