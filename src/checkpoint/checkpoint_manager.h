#pragma once

#include <tinydb/status.h>
#include "storage/page.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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
struct Policy {
  std::uint64_t wal_trigger_bytes{1U << 20U};
  std::size_t dirty_trigger_bytes{32U * PAGE_SIZE};
  std::uint64_t hard_wal_bytes{4U << 20U};
  std::size_t hard_dirty_bytes{16U << 20U};
  std::size_t failures_before_backpressure{2};
  std::chrono::steady_clock::duration maximum_age{std::chrono::seconds(30)};
};

struct Stats {
  std::size_t consecutive_failures{0};
  std::uint64_t checkpoint_lsn{0};
  bool checkpoint_requested{false};
  std::chrono::steady_clock::duration age_since_success{};
  std::optional<std::string> last_error;
};

/*
** IMMUTABLE CHECKPOINT MANAGER
**
** One manager serializes all database-file checkpoints. Capture briefly owns
** the cache/state replacement lock and retains exact immutable cache frames
** plus their DatabaseState. It releases that lock before file I/O, allowing
** readers to proceed and later transactions to publish different versions of
** the same page.
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
  Manager(DiskManager *disk, cache::CommittedPageCache *cache, txn::ReaderGate *readers, Wal *wal, Policy policy = {});

  Manager(const Manager &) = delete;
  auto operator=(const Manager &) -> Manager & = delete;

  auto Checkpoint() -> Status;
  auto ShouldCheckpoint() const -> bool;
  auto WriteAdmissionStatus() const -> Status;
  auto GetStats() const -> Stats;

 private:
  auto CheckpointLocked() -> Status;
  auto Record(Status status) -> Status;

  DiskManager *disk_;
  cache::CommittedPageCache *cache_;
  txn::ReaderGate *readers_;
  Wal *wal_;
  Policy policy_;

  mutable std::mutex checkpoint_mutex_;  // serializes capture through cleanup
  mutable std::mutex state_mutex_;       // protects failure/time diagnostics
  std::size_t consecutive_failures_{0};
  std::chrono::steady_clock::time_point last_success_{std::chrono::steady_clock::now()};
  std::optional<Status> last_failure_;
};

}  // namespace checkpoint
}  // namespace tinydb
