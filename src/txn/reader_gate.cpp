#include "txn/reader_gate.h"

#include "util/check.h"

#include <condition_variable>
#include <mutex>
#include <set>
#include <utility>

namespace tinydb::txn {

using Clock = std::chrono::steady_clock;

/*
** Shared control outlives ReaderGate whenever admitted SnapshotTokens or an
** in-progress PublicationGuard still exist. All non-atomic fields below are
** protected by mutex. changed is notified when either admission reopens or a
** reader departs, the two events on which a waiter can make progress.
*/
struct ReaderGateControl final {
  // Held only while cache pages and the state pointer are captured/replaced.
  // Checkpoints use it without entering the reader-draining state.
  std::mutex publication_mutex;
  mutable std::mutex mutex;         // protects every field below
  std::condition_variable changed;  // admission or drain may progress
  std::shared_ptr<const DatabaseState> state;
  bool publication_pending{false};  // admission is closed when true
  std::size_t active_readers{0};    // leases, not token copies

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

    /*
    ** The final shared token owns the one count decrement. The timestamp is
    ** removed under the same mutex so Stats can never observe a count and age
    ** derived from different reader populations.
    */
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

  // Capture the state and join the active set in one critical section. A
  // publisher can therefore order this reader wholly before or after itself.
  const auto started_at = Clock::now();
  control_->reader_starts.insert(started_at);
  lease->state = control_->state;
  lease->started_at = started_at;
  lease->admitted = true;
  ++control_->active_readers;
  return SnapshotToken(std::move(lease));
}

auto ReaderGate::BeginCheckpointCapture() -> CheckpointCaptureGuard { return CheckpointCaptureGuard(control_); }

auto ReaderGate::BeginPublication() noexcept -> PublicationGuard { return PublicationGuard(control_); }

auto ReaderGate::CurrentState() const -> std::shared_ptr<const DatabaseState> {
  auto lock = std::lock_guard(control_->mutex);
  return control_->state;
}

void ReaderGate::AdvanceCheckpoint(std::uint64_t checkpoint_lsn) {
  /*
  ** CHECKPOINT FRONTIER PUBLICATION
  **
  ** The database file is already durable through checkpoint_lsn. Serialize
  ** with the cache/state replacement part of normal publication, but do not
  ** drain readers: old readers may safely retain the older immutable state.
  ** Clone whichever logical state is current now and change only its
  ** persistence frontier. A transaction publishing later merges this frontier
  ** rather than restoring its older base.
  */
  auto publication_lock = std::unique_lock(control_->publication_mutex);
  auto current = std::shared_ptr<const DatabaseState>{};
  {
    auto lock = std::lock_guard(control_->mutex);
    current = control_->state;
  }
  TINYDB_CHECK(checkpoint_lsn >= current->checkpoint_lsn, "visible checkpoint frontier moved backward");
  TINYDB_CHECK(checkpoint_lsn <= current->visible_lsn, "checkpoint advanced beyond visible state");
  if (checkpoint_lsn == current->checkpoint_lsn) {
    return;
  }
  auto next = std::make_shared<DatabaseState>(*current);
  next->checkpoint_lsn = checkpoint_lsn;
  {
    auto lock = std::lock_guard(control_->mutex);
    TINYDB_CHECK(control_->state == current, "checkpoint state changed while publication was serialized");
    control_->state = std::move(next);
  }
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

CheckpointCaptureGuard::CheckpointCaptureGuard(std::shared_ptr<ReaderGateControl> control)
    : control_(std::move(control)), publication_lock_(control_->publication_mutex) {}

auto CheckpointCaptureGuard::CurrentState() const -> std::shared_ptr<const DatabaseState> {
  TINYDB_CHECK(control_ != nullptr && publication_lock_.owns_lock(), "reading an empty checkpoint capture guard");
  auto lock = std::lock_guard(control_->mutex);
  return control_->state;
}

PublicationGuard::PublicationGuard(std::shared_ptr<ReaderGateControl> control) noexcept
    : control_(std::move(control)), publication_lock_(control_->publication_mutex) {
  auto lock = std::unique_lock(control_->mutex);

  // Serializing publishers here makes the gate robust independently of the
  // future single-writer permit. Marking pending before waiting for readers is
  // the fairness boundary: subsequent BeginRead calls cannot prolong the wait.
  control_->changed.wait(lock, [this] { return !control_->publication_pending; });
  control_->publication_pending = true;
  control_->changed.wait(lock, [this] { return control_->active_readers == 0; });
}

PublicationGuard::PublicationGuard(PublicationGuard &&other) noexcept
    : control_(std::move(other.control_)), publication_lock_(std::move(other.publication_lock_)) {}

auto PublicationGuard::operator=(PublicationGuard &&other) noexcept -> PublicationGuard & {
  if (this != &other) {
    Reopen();
    control_ = std::move(other.control_);
    publication_lock_ = std::move(other.publication_lock_);
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

void PublicationGuard::Publish(std::shared_ptr<const DatabaseState> state) noexcept {
  TINYDB_CHECK(control_ != nullptr, "publishing through an empty publication guard");
  TINYDB_CHECK(state != nullptr, "publishing a null database state");
  auto lock = std::lock_guard(control_->mutex);
  TINYDB_CHECK(control_->publication_pending && control_->active_readers == 0,
               "publishing outside an exclusive visibility phase");
  // This pointer replacement is the visibility event. No admitted reader can
  // observe it halfway through because the active reader population is empty.
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
  publication_lock_.unlock();
  control_.reset();
}

}  // namespace tinydb::txn
