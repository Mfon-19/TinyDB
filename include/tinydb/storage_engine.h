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

struct TransactionCommitInfo {
  std::uint64_t transaction_id{0};
  std::uint64_t commit_lsn{0};
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
  auto CheckIntegrity() -> Status;
  auto Close() -> Status;

 private:
  explicit StorageEngine(std::shared_ptr<detail::DatabaseCore> core) : core_(std::move(core)) {}
  void CloseBestEffort() noexcept;

  std::shared_ptr<detail::DatabaseCore> core_;
};

}  // namespace tinydb
