#pragma once

#include "txn/database_state.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

namespace tinydb::txn {

class ReaderGate;
class PublicationGuard;
struct ReaderGateControl;
struct SnapshotLease;

/*
** READER ADMISSION AND PUBLICATION
**
** The gate gives many readers one immutable DatabaseState while allowing one
** publisher to replace that state atomically. Write preparation does not use
** the gate and may overlap readers.
**
** Admission has two phases:
**
**   OPEN:     BeginRead captures current state and increments active_readers.
**   DRAINING: a publisher blocks new readers and waits for the count to reach
**             zero. It may then replace committed pages and DatabaseState.
**
** Marking publication pending before waiting is the fairness boundary. A
** steady stream of new readers cannot indefinitely postpone a writer that is
** already waiting to publish.
**
** Copying SnapshotToken shares one admission. A transaction and all cursors
** created from it count as one active reader until the final token copy is
** destroyed, even if that destruction occurs on another thread.
*/
class SnapshotToken final {
 public:
  SnapshotToken() = default;

  auto State() const -> const DatabaseState &;
  auto SharedState() const -> std::shared_ptr<const DatabaseState>;
  explicit operator bool() const noexcept { return lease_ != nullptr; }

 private:
  explicit SnapshotToken(std::shared_ptr<SnapshotLease> lease) : lease_(std::move(lease)) {}

  std::shared_ptr<SnapshotLease> lease_;

  friend class ReaderGate;
};

struct ReaderGateStats {
  std::size_t active_readers{0};
  bool publication_pending{false};
  std::optional<std::chrono::steady_clock::duration> oldest_reader_age;
};

class ReaderGate final {
 public:
  explicit ReaderGate(std::shared_ptr<const DatabaseState> initial_state);

  ReaderGate(const ReaderGate &) = delete;
  auto operator=(const ReaderGate &) -> ReaderGate & = delete;

  auto BeginRead() -> SnapshotToken;
  auto BeginPublication() -> PublicationGuard;
  auto CurrentState() const -> std::shared_ptr<const DatabaseState>;
  auto Stats() const -> ReaderGateStats;

 private:
  std::shared_ptr<ReaderGateControl> control_;
};

/*
** BeginPublication returns a guard only after admission is closed and every
** previous reader has drained. The caller may replace pages and State while
** holding it. Destruction always reopens admission, including an abandoned
** pre-publication path.
*/
class PublicationGuard final {
 public:
  PublicationGuard() = default;
  PublicationGuard(const PublicationGuard &) = delete;
  auto operator=(const PublicationGuard &) -> PublicationGuard & = delete;
  PublicationGuard(PublicationGuard &&other) noexcept;
  auto operator=(PublicationGuard &&other) noexcept -> PublicationGuard &;
  ~PublicationGuard();

  auto CurrentState() const -> std::shared_ptr<const DatabaseState>;
  void Publish(std::shared_ptr<const DatabaseState> state);

 private:
  explicit PublicationGuard(std::shared_ptr<ReaderGateControl> control);
  void Reopen() noexcept;

  std::shared_ptr<ReaderGateControl> control_;

  friend class ReaderGate;
};

}  // namespace tinydb::txn
