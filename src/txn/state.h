#pragma once

#include <tinydb/status.h>

#include <cstddef>
#include <optional>

namespace tinydb::txn {

// Lifecycle describes whether the database handle admits an operation. It is
// deliberately distinct from DatabaseState, the immutable roots/LSN snapshot
// captured by a read transaction.
enum class DatabaseLifecycle {
  Open,
  CheckpointDegraded,
  NeedsRecovery,
  Corrupt,
  Closed,
};

enum class DatabaseOperation {
  Read,
  Write,
  Checkpoint,
  Backup,
  Verify,
  Stats,
  Close,
};

// This is only the state-level admission decision. An admitted operation can
// still fail for its own reasons, such as a write reaching a memory limit while
// checkpointing is degraded or Close finding a live transaction.
constexpr auto StateStatus(DatabaseLifecycle state, DatabaseOperation operation,
                           std::size_t active_transactions = 0) noexcept -> StatusCode {
  auto status = StatusCode::Corruption;
  switch (state) {
    case DatabaseLifecycle::Open:
    case DatabaseLifecycle::CheckpointDegraded:
      status = StatusCode::Ok;
      break;

    case DatabaseLifecycle::NeedsRecovery:
      if (operation == DatabaseOperation::Stats || operation == DatabaseOperation::Close) {
        status = StatusCode::Ok;
      } else {
        status = StatusCode::NeedsRecovery;
      }
      break;

    case DatabaseLifecycle::Corrupt:
      if (operation == DatabaseOperation::Verify || operation == DatabaseOperation::Stats ||
          operation == DatabaseOperation::Close) {
        status = StatusCode::Ok;
      } else {
        status = StatusCode::Corruption;
      }
      break;

    case DatabaseLifecycle::Closed:
      status = operation == DatabaseOperation::Close ? StatusCode::Ok : StatusCode::Closed;
      break;
  }

  if (status == StatusCode::Ok && operation == DatabaseOperation::Close && state != DatabaseLifecycle::Closed &&
      active_transactions != 0) {
    return StatusCode::Busy;
  }
  return status;
}

constexpr auto OpenStatus(bool database_is_owned) noexcept -> StatusCode {
  return database_is_owned ? StatusCode::Busy : StatusCode::Ok;
}

constexpr auto CanTransition(DatabaseLifecycle from, DatabaseLifecycle to) noexcept -> bool {
  switch (from) {
    case DatabaseLifecycle::Open:
      return to == DatabaseLifecycle::CheckpointDegraded || to == DatabaseLifecycle::NeedsRecovery ||
             to == DatabaseLifecycle::Corrupt || to == DatabaseLifecycle::Closed;
    case DatabaseLifecycle::CheckpointDegraded:
      return to == DatabaseLifecycle::Open || to == DatabaseLifecycle::NeedsRecovery ||
             to == DatabaseLifecycle::Corrupt || to == DatabaseLifecycle::Closed;
    case DatabaseLifecycle::NeedsRecovery:
    case DatabaseLifecycle::Corrupt:
      return to == DatabaseLifecycle::Closed;
    case DatabaseLifecycle::Closed:
      return false;
  }
  return false;
}

enum class TransactionState {
  Active,
  Frozen,
  WritingWal,
  Durable,
  Published,
  Aborted,
  Indeterminate,
};

enum class CommitOutcome {
  Committed,
  DefinitelyAborted,
  Indeterminate,
};

constexpr auto CanTransition(TransactionState from, TransactionState to) noexcept -> bool {
  switch (from) {
    case TransactionState::Active:
      return to == TransactionState::Frozen || to == TransactionState::Aborted;
    case TransactionState::Frozen:
      return to == TransactionState::WritingWal || to == TransactionState::Aborted;
    case TransactionState::WritingWal:
      return to == TransactionState::Durable || to == TransactionState::Aborted ||
             to == TransactionState::Indeterminate;
    case TransactionState::Durable:
      return to == TransactionState::Published;
    case TransactionState::Published:
    case TransactionState::Aborted:
    case TransactionState::Indeterminate:
      return false;
  }
  return false;
}

constexpr auto Outcome(TransactionState state) noexcept -> std::optional<CommitOutcome> {
  switch (state) {
    case TransactionState::Published:
      return CommitOutcome::Committed;
    case TransactionState::Aborted:
      return CommitOutcome::DefinitelyAborted;
    case TransactionState::Indeterminate:
      return CommitOutcome::Indeterminate;
    case TransactionState::Active:
    case TransactionState::Frozen:
    case TransactionState::WritingWal:
    case TransactionState::Durable:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace tinydb::txn
