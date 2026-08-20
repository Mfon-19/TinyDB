#include "checkpoint/checkpoint_manager.h"

#include "storage/disk_manager.h"
#include "storage/page.h"
#include "util/check.h"
#include "wal/wal.h"

#include "cache/committed_page_cache.h"
#include "txn/database_state.h"
#include "txn/reader_gate.h"

#include <utility>

namespace tinydb::checkpoint {
Manager::Manager(DiskManager &disk, cache::CommittedPageCache &cache, txn::ReaderGate &readers, Wal &wal,
                 CheckpointOptions options)
    : disk_(disk), cache_(cache), readers_(readers), wal_(wal), options_(options) {}

auto Manager::Record(Status status) -> Status {
  auto lock = std::lock_guard(state_mutex_);
  if (status.Ok()) {
    consecutive_failures_ = 0;
    last_success_ = std::chrono::steady_clock::now();
    last_failure_.reset();
  } else {
    ++consecutive_failures_;
    last_failure_ = status;
  }
  return status;
}

auto Manager::Checkpoint() -> Status {
  const auto state = readers_.CurrentState();
  const auto durable_lsn = disk_.CheckpointLsn();
  TINYDB_CHECK(durable_lsn <= state->visible_lsn, "database file is newer than visible state");
  auto pages = cache_.CaptureDirtyPages();

  const auto target_lsn = state->visible_lsn;
  if (target_lsn > durable_lsn) {
    // File growth and page writes precede the recovery root. Failure may leave
    // trailing physical pages, but the old superblock excludes them and keeps
    // the WAL authoritative until the new superblock has been synchronized.
    if (auto status = disk_.EnsurePageCount(state->logical_page_count); !status.Ok()) {
      return Record(std::move(status));
    }
    if (auto status = cache_.WriteCheckpointPages(pages, state->logical_page_count); !status.Ok()) {
      return Record(std::move(status));
    }
    if (auto status = disk_.Sync(); !status.Ok()) {
      return Record(std::move(status));
    }
    if (auto status = disk_.CommitCheckpoint(state->root_page_id, state->allocator_root_page_id,
                                             state->logical_page_count, target_lsn);
        !status.Ok()) {
      return Record(std::move(status));
    }
  }

  // These handles pin the exact checkpoint images during I/O. pages.clear()
  // releases them before MarkCheckpointed releases free arena memory.
  pages.clear();

  // Publish the new checkpoint LSN without draining readers; an old snapshot
  // may retain the prior LSN because its logical contents do not change.
  readers_.AdvanceCheckpoint(target_lsn);
  cache_.MarkCheckpointed(target_lsn);
  if (auto status = wal_.Reset(target_lsn); !status.Ok()) {
    return Record(std::move(status));
  }
  return Record({});
}

auto Manager::ShouldCheckpoint() const -> bool {
  const auto dirty_pages = cache_.DirtyPages();
  const auto wal_bytes = wal_.SizeBytes();
  auto lock = std::lock_guard(state_mutex_);
  const auto dirty_bytes = dirty_pages * PAGE_SIZE;
  const auto age_expired = dirty_pages != 0 && std::chrono::steady_clock::now() - last_success_ >= options_.maximum_age;
  return consecutive_failures_ != 0 || wal_bytes >= options_.wal_trigger_bytes ||
         dirty_bytes >= options_.dirty_trigger_bytes || age_expired;
}

auto Manager::WriteAdmissionStatus() const -> Status {
  auto lock = std::lock_guard(state_mutex_);
  if (consecutive_failures_ < options_.failures_before_backpressure) {
    return {};
  }
  const auto dirty_pages = cache_.DirtyPages();
  const auto wal_bytes = wal_.SizeBytes();
  const auto dirty_bytes = dirty_pages * PAGE_SIZE;
  if (wal_bytes < options_.hard_wal_bytes && dirty_bytes < options_.hard_dirty_bytes) {
    return {};
  }
  return Status::ResourceExhausted(
      "writes are paused because repeated checkpoint failure reached the WAL or dirty-memory limit");
}

auto Manager::GetStats() const -> Stats {
  const auto dirty_pages = cache_.DirtyPages();
  const auto wal_bytes = wal_.SizeBytes();
  auto lock = std::lock_guard(state_mutex_);
  const auto age_expired = dirty_pages != 0 && std::chrono::steady_clock::now() - last_success_ >= options_.maximum_age;
  return Stats{
      .consecutive_failures = consecutive_failures_,
      .checkpoint_requested = consecutive_failures_ != 0 || wal_bytes >= options_.wal_trigger_bytes ||
                              dirty_pages * PAGE_SIZE >= options_.dirty_trigger_bytes || age_expired,
      .age_since_success = std::chrono::steady_clock::now() - last_success_,
      .last_error = last_failure_ ? std::optional<std::string>{last_failure_->ToString()} : std::nullopt,
  };
}

}  // namespace tinydb::checkpoint
