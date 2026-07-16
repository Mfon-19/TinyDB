#pragma once

#include <tinydb/status.h>

#include <optional>

namespace tinydb::txn {

enum class DatabaseState {
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
constexpr auto StateStatus(DatabaseState state, DatabaseOperation operation) noexcept -> StatusCode {
  switch (state) {
    case DatabaseState::Open:
    case DatabaseState::CheckpointDegraded:
      return StatusCode::Ok;

    case DatabaseState::NeedsRecovery:
      if (operation == DatabaseOperation::Stats || operation == DatabaseOperation::Close) {
        return StatusCode::Ok;
      }
      return StatusCode::NeedsRecovery;

    case DatabaseState::Corrupt:
      if (operation == DatabaseOperation::Verify || operation == DatabaseOperation::Stats ||
          operation == DatabaseOperation::Close) {
        return StatusCode::Ok;
      }
      return StatusCode::Corruption;

    case DatabaseState::Closed:
      return operation == DatabaseOperation::Close ? StatusCode::Ok : StatusCode::Closed;
  }
  return StatusCode::Corruption;
}

constexpr auto CanTransition(DatabaseState from, DatabaseState to) noexcept -> bool {
  switch (from) {
    case DatabaseState::Open:
      return to == DatabaseState::CheckpointDegraded || to == DatabaseState::NeedsRecovery ||
             to == DatabaseState::Corrupt || to == DatabaseState::Closed;
    case DatabaseState::CheckpointDegraded:
      return to == DatabaseState::Open || to == DatabaseState::NeedsRecovery || to == DatabaseState::Corrupt ||
             to == DatabaseState::Closed;
    case DatabaseState::NeedsRecovery:
    case DatabaseState::Corrupt:
      return to == DatabaseState::Closed;
    case DatabaseState::Closed:
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
  Aborted,
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
      return CommitOutcome::Aborted;
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
