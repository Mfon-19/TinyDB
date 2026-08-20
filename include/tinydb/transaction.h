#pragma once

#include <tinydb/bytes.h>
#include <tinydb/cursor.h>
#include <tinydb/status.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace tinydb {

class Database;

struct CommitInfo final {
  std::uint64_t commit_lsn{0};
};

/*
** PUBLIC TRANSACTION HANDLES
**
** ReadTransaction owns one immutable-root admission. WriteTransaction owns
** the process-local writer permit and a private page overlay. Both keep the
** database core alive across Database moves.
**
** Readers need no tree-level lock after admission because their DatabaseState
** and committed page versions are immutable. The single writer uses a
** page-level copy-on-write overlay, so abort is discard rather than undo and
** no write-write conflict resolution is needed. Its new root and page versions
** become reachable only after WAL durability and quiescent-state publication.
**
** Destroying an active writer aborts it. Commit consumes its handle: success
** means the complete transaction is WAL-durable and visible. An
** IndeterminateCommit or NeedsRecovery result requires reopening the database
** before the application decides whether the transaction committed.
*/
class ReadTransaction final {
 public:
  ReadTransaction(const ReadTransaction &) = delete;
  auto operator=(const ReadTransaction &) -> ReadTransaction & = delete;
  ReadTransaction(ReadTransaction &&) noexcept;
  auto operator=(ReadTransaction &&) -> ReadTransaction & = delete;
  ~ReadTransaction();

  auto Get(BytesView key) -> Result<std::optional<Bytes>>;
  auto Scan(KeyRange range = KeyRange::All()) -> Result<Cursor>;

 private:
  struct Impl;
  explicit ReadTransaction(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class Database;
};

class WriteTransaction final {
 public:
  WriteTransaction(const WriteTransaction &) = delete;
  auto operator=(const WriteTransaction &) -> WriteTransaction & = delete;
  WriteTransaction(WriteTransaction &&) noexcept;
  auto operator=(WriteTransaction &&) -> WriteTransaction & = delete;
  ~WriteTransaction();

  auto Get(BytesView key) -> Result<std::optional<Bytes>>;
  auto Put(BytesView key, BytesView value) -> Status;
  auto Delete(BytesView key) -> Status;
  auto Commit() && -> Result<CommitInfo>;
  void Abort() noexcept;

 private:
  struct Impl;
  explicit WriteTransaction(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class Database;
};

}  // namespace tinydb
