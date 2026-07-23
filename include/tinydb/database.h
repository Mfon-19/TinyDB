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

class DatabaseTestAccess;
namespace detail {
class DatabaseCore;
}

/*
** EMBEDDED DATABASE HANDLE
**
** Database owns one process-exclusive open of a database file. Its private
** core owns every storage subsystem at stable addresses; public transaction
** and cursor handles retain that core while they borrow it.
** Convenience reads and writes use the same snapshot and commit paths exposed
** through transaction objects, so there is only one visibility and durability
** protocol.
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

  // Audits one stable read snapshot without repair or persistent writes.
  // Detected damage is represented by VerifyReport::issues; Result errors are
  // reserved for environmental failures that prevented the audit.
  auto Verify(VerifyOptions options = {}) -> Result<VerifyReport>;

  // Returns race-free subsystem counters for diagnosis, not one transactional
  // snapshot spanning every independently synchronized subsystem.
  auto Stats() const -> Result<DatabaseStats>;
  auto Close() -> Status;

 private:
  friend class DatabaseTestAccess;

  explicit Database(std::shared_ptr<detail::DatabaseCore> core);

  std::shared_ptr<detail::DatabaseCore> core_;
};

}  // namespace tinydb
