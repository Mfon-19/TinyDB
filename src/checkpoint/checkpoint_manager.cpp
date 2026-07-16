#include "checkpoint/checkpoint_manager.h"

#include <tinydb/check.h>
#include <tinydb/disk_manager.h>
#include <tinydb/page.h>
#include <tinydb/wal.h>

#include "cache/committed_page_cache.h"
#include "txn/database_state.h"
#include "txn/reader_gate.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace tinydb::checkpoint {
namespace {

auto DirtyBytes(const cache::CommittedCacheStats &stats) -> std::size_t {
  if (stats.dirty_pages > std::numeric_limits<std::size_t>::max() / PAGE_SIZE) {
    return std::numeric_limits<std::size_t>::max();
  }
  return stats.dirty_pages * PAGE_SIZE;
}

}  // namespace

Manager::Manager(DiskManager *disk, cache::CommittedPageCache *cache, txn::ReaderGate *readers, Wal *wal, Policy policy)
    : disk_(disk), cache_(cache), readers_(readers), wal_(wal), policy_(policy) {
  TINYDB_CHECK(disk_ != nullptr && cache_ != nullptr && readers_ != nullptr && wal_ != nullptr,
               "checkpoint manager requires every storage domain");
  TINYDB_CHECK(policy_.wal_trigger_bytes != 0 && policy_.dirty_trigger_bytes != 0 &&
                   policy_.hard_wal_bytes >= policy_.wal_trigger_bytes &&
                   policy_.hard_dirty_bytes >= policy_.dirty_trigger_bytes && policy_.failures_before_backpressure != 0,
               "checkpoint policy has invalid pressure thresholds");
}

auto Manager::Record(Status status) -> Status {
  auto lock = std::lock_guard(state_mutex_);
  if (status.Ok()) {
    consecutive_failures_ = 0;
    last_success_ = std::chrono::steady_clock::now();
  } else {
    if (consecutive_failures_ != std::numeric_limits<std::size_t>::max()) {
      ++consecutive_failures_;
    }
  }
  return status;
}

auto Manager::Checkpoint() -> Status {
  // The mutex spans capture, I/O, and cleanup. Two checkpoints can otherwise
  // write old and new page versions out of order even though each snapshot is
  // internally valid.
  auto checkpoint_lock = std::lock_guard(checkpoint_mutex_);

  auto state = std::shared_ptr<const txn::DatabaseState>{};
  auto pages = std::vector<cache::PageGuard>{};
  {
    /*
    ** CAPTURE PHASE
    **
    ** CheckpointCaptureGuard supplies an atomic boundary between cache
    ** installation and DatabaseState replacement. Existing readers are
    ** irrelevant to page lifetime, while the capture lock ensures no publisher
    ** changes the dense page table as these exact current versions are selected.
    */
    auto capture = readers_->BeginCheckpointCapture();
    state = capture.CurrentState();
    const auto durable_lsn = disk_->CheckpointLsn();
    TINYDB_CHECK(durable_lsn <= state->visible_lsn, "database file is newer than visible state");
    pages = cache_->CaptureCheckpointPages(durable_lsn, state->visible_lsn);
  }

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
  ** A newer transaction may have published during I/O. AdvanceCheckpoint
  ** clones that current state and changes only its checkpoint frontier. Cache
  ** marking similarly tests each current frame's LSN, so P@N+1 remains dirty
  ** when this checkpoint wrote the retained P@N guard.
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
      cache.dirty_pages != 0 && std::chrono::steady_clock::now() - last_success_ >= policy_.maximum_age;
  return consecutive_failures_ != 0 || wal_bytes >= policy_.wal_trigger_bytes ||
         dirty_bytes >= policy_.dirty_trigger_bytes || age_expired;
}

auto Manager::WriteAdmissionStatus() const -> Status {
  const auto cache = cache_->Stats();
  const auto wal_bytes = wal_->SizeBytes();
  auto lock = std::lock_guard(state_mutex_);
  if (consecutive_failures_ < policy_.failures_before_backpressure) {
    return {};
  }
  const auto dirty_bytes = DirtyBytes(cache);
  if (wal_bytes < policy_.hard_wal_bytes && dirty_bytes < policy_.hard_dirty_bytes) {
    return {};
  }
  return Status::ResourceExhausted(
      "writes are paused because repeated checkpoint failure reached the WAL or dirty-memory limit");
}

auto Manager::GetStats() const -> Stats {
  const auto state = readers_->CurrentState();
  auto lock = std::lock_guard(state_mutex_);
  return Stats{
      .consecutive_failures = consecutive_failures_,
      .checkpoint_lsn = state->checkpoint_lsn,
      .checkpoint_requested = consecutive_failures_ != 0 || wal_->SizeBytes() >= policy_.wal_trigger_bytes ||
                              DirtyBytes(cache_->Stats()) >= policy_.dirty_trigger_bytes,
  };
}

}  // namespace tinydb::checkpoint
