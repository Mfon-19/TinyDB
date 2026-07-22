#include "checkpoint/checkpoint_manager.h"

#include "storage/disk_manager.h"
#include "storage/page.h"
#include "util/check.h"
#include "wal/wal.h"

#include "cache/committed_page_cache.h"
#include "txn/database_state.h"
#include "txn/reader_gate.h"

#include <limits>
#include <utility>

namespace tinydb::checkpoint {
namespace {

auto DirtyBytes(const cache::CommittedCacheStats &stats) -> std::size_t {
  if (stats.dirty_pages > std::numeric_limits<std::size_t>::max() / PAGE_SIZE) {
    return std::numeric_limits<std::size_t>::max();
  }
  return stats.dirty_pages * PAGE_SIZE;
}

}  // namespace

Manager::Manager(DiskManager *disk, cache::CommittedPageCache *cache, txn::ReaderGate *readers, Wal *wal,
                 CheckpointOptions options)
    : disk_(disk), cache_(cache), readers_(readers), wal_(wal), options_(options) {
  TINYDB_CHECK(disk_ != nullptr && cache_ != nullptr && readers_ != nullptr && wal_ != nullptr,
               "checkpoint manager requires every storage domain");
  TINYDB_CHECK(options_.wal_trigger_bytes != 0 && options_.dirty_trigger_bytes != 0 &&
                   options_.hard_wal_bytes >= options_.wal_trigger_bytes &&
                   options_.hard_dirty_bytes >= options_.dirty_trigger_bytes &&
                   options_.failures_before_backpressure != 0,
               "checkpoint policy has invalid pressure thresholds");
}

auto Manager::Record(Status status) -> Status {
  auto lock = std::lock_guard(state_mutex_);
  if (status.Ok()) {
    consecutive_failures_ = 0;
    last_success_ = std::chrono::steady_clock::now();
    last_failure_.reset();
  } else {
    if (consecutive_failures_ != std::numeric_limits<std::size_t>::max()) {
      ++consecutive_failures_;
    }
    last_failure_ = status;
  }
  return status;
}

auto Manager::Checkpoint() -> Status {
  // The DatabaseCore writer permit excludes publication throughout capture,
  // I/O, and cleanup. Reader activity cannot replace immutable current pages.
  const auto state = readers_->CurrentState();
  const auto durable_lsn = disk_->CheckpointLsn();
  TINYDB_CHECK(durable_lsn <= state->visible_lsn, "database file is newer than visible state");
  auto pages = cache_->CaptureCheckpointPages(durable_lsn, state->visible_lsn);

  const auto target_lsn = state->visible_lsn;
  if (target_lsn > disk_->CheckpointLsn()) {
    // File growth is harmless before the recovery root advances. A failure can
    // leave trailing zero pages, but the old high-water frontier ignores them.
    if (auto status = disk_->EnsurePageCount(state->high_water_page_id); !status.Ok()) {
      return Record(std::move(status));
    }
    for (const auto &page : pages) {
      TINYDB_CHECK(page.PageLsn() <= target_lsn, "checkpoint captured a future page version");
      if (auto status = disk_->WriteCheckpointPage(page.Id(), page.Data().data(), state->high_water_page_id);
          !status.Ok()) {
        return Record(std::move(status));
      }
    }
    if (auto status = disk_->Sync(); !status.Ok()) {
      return Record(std::move(status));
    }
    if (auto status = disk_->CommitCheckpoint(state->root_page_id, state->allocator_root_page_id,
                                              state->high_water_page_id, state->transaction_id, target_lsn);
        !status.Ok()) {
      return Record(std::move(status));
    }
  }

  /*
  ** POST-DURABILITY PHASE
  **
  ** Publish the completed persistence frontier without draining readers. Old
  ** snapshots may retain the prior frontier, which does not affect their
  ** logical contents.
  */
  readers_->AdvanceCheckpoint(target_lsn);
  cache_->MarkCheckpointed(target_lsn);
  if (auto status = wal_->CleanupCheckpointed(target_lsn); !status.Ok()) {
    return Record(std::move(status));
  }
  return Record({});
}

auto Manager::ShouldCheckpoint() const -> bool {
  const auto cache = cache_->Stats();
  const auto wal_bytes = wal_->SizeBytes();
  auto lock = std::lock_guard(state_mutex_);
  const auto dirty_bytes = DirtyBytes(cache);
  const auto age_expired =
      cache.dirty_pages != 0 && std::chrono::steady_clock::now() - last_success_ >= options_.maximum_age;
  return consecutive_failures_ != 0 || wal_bytes >= options_.wal_trigger_bytes ||
         dirty_bytes >= options_.dirty_trigger_bytes || age_expired;
}

auto Manager::WriteAdmissionStatus() const -> Status {
  auto lock = std::lock_guard(state_mutex_);
  if (consecutive_failures_ < options_.failures_before_backpressure) {
    return {};
  }
  const auto cache = cache_->Stats();
  const auto wal_bytes = wal_->SizeBytes();
  const auto dirty_bytes = DirtyBytes(cache);
  if (wal_bytes < options_.hard_wal_bytes && dirty_bytes < options_.hard_dirty_bytes) {
    return {};
  }
  return Status::ResourceExhausted(
      "writes are paused because repeated checkpoint failure reached the WAL or dirty-memory limit");
}

auto Manager::GetStats() const -> Stats {
  const auto cache = cache_->Stats();
  const auto wal_bytes = wal_->SizeBytes();
  auto lock = std::lock_guard(state_mutex_);
  const auto age_expired =
      cache.dirty_pages != 0 && std::chrono::steady_clock::now() - last_success_ >= options_.maximum_age;
  return Stats{
      .consecutive_failures = consecutive_failures_,
      .checkpoint_requested = consecutive_failures_ != 0 || wal_bytes >= options_.wal_trigger_bytes ||
                              DirtyBytes(cache) >= options_.dirty_trigger_bytes || age_expired,
      .age_since_success = std::chrono::steady_clock::now() - last_success_,
      .last_error = last_failure_ ? std::optional<std::string>{last_failure_->ToString()} : std::nullopt,
  };
}

}  // namespace tinydb::checkpoint
