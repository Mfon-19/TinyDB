#pragma once

#include <tinydb/options.h>
#include <tinydb/status.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace tinydb {

class DiskManager;
class Wal;

namespace cache {
class CommittedPageCache;
}

namespace txn {
class ReaderGate;
}

namespace checkpoint {

/*
** CHECKPOINT POLICY
**
** Soft thresholds request maintenance before admitting another writer. Hard
** thresholds apply backpressure only after repeated checkpoint failure, which
** bounds WAL and dirty-cache growth without making one transient I/O error a
** write outage. The elapsed trigger is evaluated when the database next has an
** opportunity to run maintenance; TinyDB does not own a background thread.
*/
struct Stats {
  std::size_t consecutive_failures{0};
  bool checkpoint_requested{false};
  std::chrono::steady_clock::duration age_since_success{};
  std::optional<std::string> last_error;
};

/*
** IMMUTABLE CHECKPOINT MANAGER
**
** The database's writer permit serializes checkpoints with transactions.
** Readers continue during checkpoint I/O because committed frames and their
** DatabaseState are immutable.
**
** Persistent order is:
**
**   extend file -> write captured pages -> fsync data
**   -> write inactive superblock -> fsync superblock
**   -> advance in-memory frontier -> mark cache versions checkpointed
**   -> remove covered immutable WAL segments -> fsync WAL directory
**
** Failure before the superblock fsync leaves the old superblock and WAL
** authoritative. Failure afterward may leave redundant WAL history, but the
** new checkpoint is already a complete recovery basis.
*/
class Manager final {
 public:
  Manager(DiskManager *disk, cache::CommittedPageCache *cache, txn::ReaderGate *readers, Wal *wal,
          CheckpointOptions options = {});

  Manager(const Manager &) = delete;
  auto operator=(const Manager &) -> Manager & = delete;

  auto Checkpoint() -> Status;
  auto ShouldCheckpoint() const -> bool;
  auto WriteAdmissionStatus() const -> Status;
  auto GetStats() const -> Stats;

 private:
  auto Record(Status status) -> Status;

  DiskManager *disk_;
  cache::CommittedPageCache *cache_;
  txn::ReaderGate *readers_;
  Wal *wal_;
  CheckpointOptions options_;

  mutable std::mutex state_mutex_;  // protects failure/time diagnostics
  std::size_t consecutive_failures_{0};
  std::chrono::steady_clock::time_point last_success_{std::chrono::steady_clock::now()};
  std::optional<Status> last_failure_;
};

}  // namespace checkpoint
}  // namespace tinydb
