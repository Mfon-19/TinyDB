#pragma once

#include <tinydb/status.h>

#include <cstddef>
#include <optional>

namespace tinydb::txn {

/*
** DATABASE AND TRANSACTION STATE MACHINES
**
** DatabaseLifecycle answers whether a handle may admit a kind of operation.
** It is intentionally different from DatabaseState, which is the immutable
** root and LSN snapshot captured by readers.
**
** Database lifecycle transitions:
**
**   Open <--------> CheckpointDegraded
**     |                       |
**     +----> NeedsRecovery <--+
**     +----> Corrupt <--------+
**     +----> Closed <---------+
**
** NeedsRecovery and Corrupt are terminal except for Close. Reopening creates
** a new handle only after recovery or verification establishes trustworthy
** state.
**
** Write transaction transitions:
**
**   Active -> Frozen -> WritingWal -> Durable -> Published
**      |         |           |
**      +---------+-----------+-> Aborted
**                            +-> Indeterminate
**
** Durable has exactly one legal successor. Once WAL synchronization proves a
** transaction durable, publication must be prepared already and cannot be
** abandoned. Only terminal transaction states have a CommitOutcome.
*/
enum class DatabaseLifecycle {
  Open,                // all operations admitted subject to normal conflicts
  CheckpointDegraded,  // reads/writes continue; WAL/cache pressure may block
  NeedsRecovery,       // commit outcome or durable tail cannot be trusted live
  Corrupt,             // persistent state was structurally invalid
  Closed,              // resources released; only repeated Close is valid
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

/*
** Return the state-level admission decision only. An admitted operation may
** still fail for an operation-specific reason, such as memory exhaustion.
** Close is additionally Busy while any transaction retains its admission.
*/
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
  Active,         // application and tree mutations are still permitted
  Frozen,         // final images/state are immutable but not yet appended
  WritingWal,     // durable outcome has not yet been established
  Durable,        // commit record crossed the synchronization boundary
  Published,      // durable state is also the visible committed state
  Aborted,        // definitely absent from durable and visible state
  Indeterminate,  // caller must reopen to discover whether commit survived
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
