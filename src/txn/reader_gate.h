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

// Copying a token shares one reader admission. A transaction and all cursors
// created from it therefore count as one active reader until the final copy is
// destroyed, regardless of which thread performs that destruction.
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

// Coordinates snapshot admission with the short publication phase. Preparing
// a write does not touch this gate; only publication closes admission and
// waits for readers that captured the previous committed state.
class ReaderGate final {
 public:
  explicit ReaderGate(std::shared_ptr<const DatabaseState> initial_state);

  ReaderGate(const ReaderGate &) = delete;
  auto operator=(const ReaderGate &) -> ReaderGate & = delete;

  auto BeginRead() -> SnapshotToken;
  auto BeginPublication() -> PublicationGuard;
  auto Stats() const -> ReaderGateStats;

 private:
  std::shared_ptr<ReaderGateControl> control_;
};

// BeginPublication returns with admission closed and every previous reader
// drained. The caller may splice cache pages and replace State while holding
// this guard; destruction always reopens admission, including failure paths.
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
