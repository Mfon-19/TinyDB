#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace tinydb {

/*
** Redo-only write-ahead log containing final physical page images.
**
** AppendPageImage builds an in-memory transaction. Commit appends every image
** plus its binding commit record and synchronizes once; that synchronization
** is the mutation durability point. Reset is legal only after the caller has
** written and synchronized the same state into the database file.
**
** Recover is the only path that interprets non-empty durable record history.
** Open requires a clean header-only log, preventing normal operation from
** accidentally bypassing crash recovery.
*/
class Wal {
 public:
  // The companion path is deterministic so recovery can run before the
  // database file is parsed or a normal Wal object exists.
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;

  // Replays complete committed runs and ignores only an incomplete trailing
  // run. Successful recovery leaves the database durable before truncating
  // the WAL back to its header.
  static auto Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status;

  // Creates or validates a clean header-only WAL bound to database_uuid.
  static auto Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;

  Wal(Wal &&) noexcept = default;
  auto operator=(Wal &&) noexcept -> Wal & = default;
  ~Wal() = default;

  void AppendPageImage(page_id_t page_id, const char *data);

  // Used when a mutation fails before commit. This drops only RAM bytes; it
  // never removes previously committed records from the durable log.
  void DiscardPending();
  auto Commit() -> Status;
  auto SizeBytes() const -> std::uint64_t;
  auto Reset() -> Status;

 private:
  Wal(UniqueFd fd, std::uint64_t size_bytes) : fd_(std::move(fd)), size_bytes_(size_bytes) {}

  UniqueFd fd_;

  // Durable append offset, excluding pending_ until fsync succeeds.
  std::uint64_t size_bytes_{0};

  // Transaction IDs are monotonic within this WAL lifetime. Recovery also
  // requires all records in a run to carry one identical nonzero ID.
  std::uint64_t next_transaction_id_{1};

  // Number is tracked separately from bytes because the COMMIT payload binds
  // both the record digest and the expected image count.
  std::size_t pending_image_count_{0};

  // Complete encoded page-image records awaiting one contiguous append.
  std::vector<char> pending_;
};
}  // namespace tinydb
