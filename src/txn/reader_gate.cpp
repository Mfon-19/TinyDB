#include "txn/reader_gate.h"

#include <tinydb/check.h>

#include <condition_variable>
#include <mutex>
#include <set>
#include <utility>

namespace tinydb::txn {

using Clock = std::chrono::steady_clock;

struct ReaderGateControl final {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<const DatabaseState> state;
  bool publication_pending{false};
  std::size_t active_readers{0};

  // A multiset preserves exact oldest-reader diagnostics even when several
  // transactions begin during the same clock tick.
  std::multiset<Clock::time_point> reader_starts;
};

struct SnapshotLease final {
  std::shared_ptr<ReaderGateControl> control;
  std::shared_ptr<const DatabaseState> state;
  Clock::time_point started_at{};
  bool admitted{false};

  ~SnapshotLease() {
    if (!admitted) {
      return;
    }

    auto lock = std::lock_guard(control->mutex);
    TINYDB_CHECK(control->active_readers != 0, "reader gate active count underflow");
    const auto start = control->reader_starts.find(started_at);
    TINYDB_CHECK(start != control->reader_starts.end(), "reader gate lost a reader timestamp");
    control->reader_starts.erase(start);
    --control->active_readers;

    // A publisher only needs the zero transition, but notifying on every
    // release also keeps the condition independent of that optimization.
    control->changed.notify_all();
  }
};

auto SnapshotToken::State() const -> const DatabaseState & {
  TINYDB_CHECK(lease_ != nullptr, "reading an empty snapshot token");
  return *lease_->state;
}

auto SnapshotToken::SharedState() const -> std::shared_ptr<const DatabaseState> {
  TINYDB_CHECK(lease_ != nullptr, "reading an empty snapshot token");
  return lease_->state;
}

ReaderGate::ReaderGate(std::shared_ptr<const DatabaseState> initial_state)
    : control_(std::make_shared<ReaderGateControl>()) {
  TINYDB_CHECK(initial_state != nullptr, "reader gate requires an initial database state");
  control_->state = std::move(initial_state);
}

auto ReaderGate::BeginRead() -> SnapshotToken {
  // Allocate the shared lease before admission. If allocation fails, the gate
  // has not incremented its reader count and needs no rollback.
  auto lease = std::make_shared<SnapshotLease>();
  lease->control = control_;

  auto lock = std::unique_lock(control_->mutex);
  control_->changed.wait(lock, [this] { return !control_->publication_pending; });

  const auto started_at = Clock::now();
  control_->reader_starts.insert(started_at);
  lease->state = control_->state;
  lease->started_at = started_at;
  lease->admitted = true;
  ++control_->active_readers;
  return SnapshotToken(std::move(lease));
}

auto ReaderGate::BeginPublication() -> PublicationGuard { return PublicationGuard(control_); }

auto ReaderGate::CurrentState() const -> std::shared_ptr<const DatabaseState> {
  auto lock = std::lock_guard(control_->mutex);
  return control_->state;
}

auto ReaderGate::Stats() const -> ReaderGateStats {
  auto lock = std::lock_guard(control_->mutex);
  auto result = ReaderGateStats{
      .active_readers = control_->active_readers,
      .publication_pending = control_->publication_pending,
      .oldest_reader_age = std::nullopt,
  };
  if (!control_->reader_starts.empty()) {
    result.oldest_reader_age = Clock::now() - *control_->reader_starts.begin();
  }
  return result;
}

PublicationGuard::PublicationGuard(std::shared_ptr<ReaderGateControl> control) : control_(std::move(control)) {
  auto lock = std::unique_lock(control_->mutex);

  // Serializing publishers here makes the gate robust independently of the
  // future single-writer permit. Marking pending before waiting for readers is
  // the fairness boundary: subsequent BeginRead calls cannot prolong the wait.
  control_->changed.wait(lock, [this] { return !control_->publication_pending; });
  control_->publication_pending = true;
  control_->changed.wait(lock, [this] { return control_->active_readers == 0; });
}

PublicationGuard::PublicationGuard(PublicationGuard &&other) noexcept : control_(std::move(other.control_)) {}

auto PublicationGuard::operator=(PublicationGuard &&other) noexcept -> PublicationGuard & {
  if (this != &other) {
    Reopen();
    control_ = std::move(other.control_);
  }
  return *this;
}

PublicationGuard::~PublicationGuard() { Reopen(); }

auto PublicationGuard::CurrentState() const -> std::shared_ptr<const DatabaseState> {
  TINYDB_CHECK(control_ != nullptr, "reading an empty publication guard");
  auto lock = std::lock_guard(control_->mutex);
  TINYDB_CHECK(control_->publication_pending && control_->active_readers == 0,
               "publication guard does not own an exclusive visibility phase");
  return control_->state;
}

void PublicationGuard::Publish(std::shared_ptr<const DatabaseState> state) {
  TINYDB_CHECK(control_ != nullptr, "publishing through an empty publication guard");
  TINYDB_CHECK(state != nullptr, "publishing a null database state");
  auto lock = std::lock_guard(control_->mutex);
  TINYDB_CHECK(control_->publication_pending && control_->active_readers == 0,
               "publishing outside an exclusive visibility phase");
  control_->state = std::move(state);
}

void PublicationGuard::Reopen() noexcept {
  if (control_ == nullptr) {
    return;
  }
  {
    auto lock = std::lock_guard(control_->mutex);
    TINYDB_CHECK(control_->publication_pending, "reopening reader admission that is already open");
    control_->publication_pending = false;
  }
  control_->changed.notify_all();
  control_.reset();
}

}  // namespace tinydb::txn
