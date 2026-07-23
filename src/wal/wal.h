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
** SINGLE-FILE REDO LOG
**
** Prepare encodes final data-page images without touching the filesystem.
** Commit appends that owned transaction and synchronizes once; the fsync is
** the public durability point. After a checkpoint has synchronized the same
** pages and its new superblock, Reset replaces the covered log with one clean
** header. There are no archive files or segment lifecycle.
*/
class Wal {
 public:
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;

  // Recovery leaves a header-only WAL whose starting LSN is the checkpoint
  // frontier plus one. Open creates that header for a new database.
  static auto Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
                   std::uint64_t starting_lsn) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;
  Wal(Wal &&other) noexcept;
  auto operator=(Wal &&) -> Wal & = delete;
  ~Wal() = default;

  auto NextCommitLsn() const -> Result<std::uint64_t>;
  auto Prepare(std::span<const wal_format::PageImageView> pages,
               txn::DatabaseState state) const -> Result<wal_format::EncodedTransaction>;
  auto Commit(wal_format::EncodedTransaction transaction) -> Result<std::uint64_t>;

  auto SizeBytes() const -> std::uint64_t;

  // The caller has already made checkpoint_lsn durable in the database
  // superblock and excludes writers. A failure forbids further appends until
  // reopen, while the durable database remains a complete recovery base.
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
