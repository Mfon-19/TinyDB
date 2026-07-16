#pragma once

#include <tinydb/bytes.h>
#include <tinydb/options.h>
#include <tinydb/stats.h>
#include <tinydb/status.h>
#include <tinydb/transaction.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace tinydb {

/*
** EMBEDDED DATABASE HANDLE
**
** Database owns one process-exclusive open of a database file. Its private
** implementation owns every storage subsystem at stable addresses; public
** transaction and cursor handles retain the core objects they borrow.
** Convenience reads and writes create the same transaction objects exposed to
** applications, so there is only one visibility and durability path.
*/
class Database final {
 public:
  Database(const Database &) = delete;
  auto operator=(const Database &) -> Database & = delete;
  Database(Database &&) noexcept;
  auto operator=(Database &&) -> Database & = delete;
  ~Database();

  static auto Open(const std::filesystem::path &path, Options options = {}) -> Result<Database>;

  auto BeginRead() -> Result<ReadTransaction>;
  auto BeginWrite() -> Result<WriteTransaction>;

  auto Put(BytesView key, BytesView value) -> Status;
  auto Get(BytesView key) -> Result<std::optional<Bytes>>;
  auto Delete(BytesView key) -> Status;

  // Commits are durable before this call. Checkpoint only makes the visible
  // state self-contained in the database file and shortens future recovery.
  auto Checkpoint() -> Status;
  auto Stats() const -> Result<DatabaseStats>;
  auto Close() -> Status;

 private:
  struct Impl;
  explicit Database(std::unique_ptr<Impl> impl);
  void CloseBestEffort() noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace tinydb
