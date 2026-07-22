#pragma once

#include <tinydb/status.h>

#include <cstddef>

namespace tinydb::txn {

/*
** DATABASE LIFECYCLE
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
** Commit sequencing is enforced directly by CommitCoordinator: after WAL
** synchronization, only its prepared no-fail publication path remains.
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

}  // namespace tinydb::txn
