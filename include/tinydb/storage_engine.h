#pragma once

#include <tinydb/limits.h>
#include <tinydb/status.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

namespace detail {
class DatabaseCore;
}

/*
** ORDERED RANGE DESCRIPTION
**
** Bounds are owned because a Cursor may outlive the strings used to create
** it. Between is half-open: [lower, upper). From and Until omit one bound.
** Prefix computes the smallest exclusive bytewise successor when one exists;
** an all-0xff prefix has no upper bound.
*/
class KeyRange final {
 public:
  KeyRange() = default;

  static auto All() -> KeyRange;
  static auto From(std::string_view lower) -> KeyRange;
  static auto Until(std::string_view upper) -> KeyRange;
  static auto Between(std::string_view lower, std::string_view upper) -> KeyRange;
  static auto Prefix(std::string_view prefix) -> KeyRange;

  auto Lower() const -> std::optional<std::string_view>;
  auto Upper() const -> std::optional<std::string_view>;

 private:
  KeyRange(std::optional<std::string> lower, std::optional<std::string> upper)
      : lower_(std::move(lower)), upper_(std::move(upper)) {}

  std::optional<std::string> lower_;
  std::optional<std::string> upper_;
};

struct TransactionCommitInfo {
  std::uint64_t transaction_id{0};
  std::uint64_t commit_lsn{0};
};

/*
** A Cursor is one streaming view of a read snapshot. Key borrows the currently
** pinned leaf and remains valid until First, Seek, Next, move-assignment, or
** destruction. CopyValue returns owned bytes. Reaching the end of the range is
** represented by Valid()==false after a successful movement operation.
**
** The cursor retains both the snapshot admission and DatabaseCore lifetime.
** It may outlive the ReadTransaction that created it, and Close returns Busy
** until the cursor is destroyed.
*/
class Cursor final {
 public:
  Cursor(const Cursor &) = delete;
  auto operator=(const Cursor &) -> Cursor & = delete;
  Cursor(Cursor &&) noexcept;
  auto operator=(Cursor &&) noexcept -> Cursor &;
  ~Cursor();

  auto First() -> Status;
  auto Seek(std::string_view key) -> Status;
  auto Next() -> Status;
  auto Valid() const -> bool;
  auto Key() const -> std::string_view;
  auto ValueSize() const -> std::uint64_t;
  auto CopyValue() const -> Result<std::string>;

 private:
  struct Impl;
  explicit Cursor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class ReadTransaction;
};

/*
** PUBLIC TRANSACTION HANDLES
**
** ReadTransaction owns one reader admission and therefore one immutable root
** snapshot. WriteTransaction owns the process-local writer permit and a private
** page overlay. Both keep DatabaseCore alive across StorageEngine moves.
**
** Destruction aborts an active writer. Commit success means the complete
** transaction is both WAL-durable and visible. IndeterminateCommit means the
** caller must reopen the database and inspect application state before it can
** know whether the transaction committed.
*/
class ReadTransaction final {
 public:
  ReadTransaction(const ReadTransaction &) = delete;
  auto operator=(const ReadTransaction &) -> ReadTransaction & = delete;
  ReadTransaction(ReadTransaction &&) noexcept;
  auto operator=(ReadTransaction &&) noexcept -> ReadTransaction &;
  ~ReadTransaction();

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Scan(KeyRange range = KeyRange::All()) -> Result<Cursor>;

 private:
  struct Impl;
  explicit ReadTransaction(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class StorageEngine;
  friend class detail::DatabaseCore;
};

class WriteTransaction final {
 public:
  WriteTransaction(const WriteTransaction &) = delete;
  auto operator=(const WriteTransaction &) -> WriteTransaction & = delete;
  WriteTransaction(WriteTransaction &&) noexcept;
  auto operator=(WriteTransaction &&) noexcept -> WriteTransaction &;
  ~WriteTransaction();

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Put(std::string_view key, std::string_view value) -> Status;
  auto Delete(std::string_view key) -> Status;
  auto Commit() -> Result<TransactionCommitInfo>;
  void Abort() noexcept;

 private:
  struct Impl;
  explicit WriteTransaction(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class StorageEngine;
  friend class detail::DatabaseCore;
};

/*
** StorageEngine is the owning database handle. Convenience reads and writes
** create the same public transaction objects applications use directly; there
** is no second single-operation commit path.
*/
class StorageEngine final {
 public:
  StorageEngine(const StorageEngine &) = delete;
  auto operator=(const StorageEngine &) -> StorageEngine & = delete;
  StorageEngine(StorageEngine &&) noexcept = default;
  auto operator=(StorageEngine &&) noexcept -> StorageEngine & = default;
  ~StorageEngine();

  static auto Open(const std::filesystem::path &path) -> Result<StorageEngine>;

  auto BeginRead() -> Result<ReadTransaction>;
  auto BeginWrite() -> Result<WriteTransaction>;

  auto Put(std::string_view key, std::string_view value) -> Status;
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Remove(std::string_view key) -> Status;
  auto Scan(std::string_view start, std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>>;

  // Makes the current visible state self-contained in the database file.
  // Commits are already durable through WAL; checkpoint failure therefore
  // leaves acknowledged transactions valid and may be retried.
  auto Checkpoint() -> Status;
  auto CheckIntegrity() -> Status;
  auto Close() -> Status;

 private:
  explicit StorageEngine(std::shared_ptr<detail::DatabaseCore> core) : core_(std::move(core)) {}
  void CloseBestEffort() noexcept;

  std::shared_ptr<detail::DatabaseCore> core_;
};

}  // namespace tinydb
