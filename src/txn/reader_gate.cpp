#include "txn/reader_gate.h"

#include "util/check.h"

#include <condition_variable>
#include <mutex>
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
  mutable std::mutex mutex;         // protects every field below
  std::condition_variable changed;  // admission or drain may progress
  std::shared_ptr<const DatabaseState> state;
  bool publication_pending{false};  // admission is closed when true

  // Admissions append in clock order. Intrusive links live either in the
  // shared cursor lease or in a point read's stack token, avoiding a separate
  // diagnostic allocation.
  ReaderGateAdmission *oldest_reader{nullptr};
  ReaderGateAdmission *newest_reader{nullptr};
  std::size_t active_readers{0};
};

struct SnapshotLease final {
  ReaderGateAdmission admission;
};

ReaderGateAdmission::~ReaderGateAdmission() {
  if (!admitted) {
    return;
  }

  /*
  ** The owner of this admission performs the one unlink. Count and oldest age
  ** change under the same mutex, so diagnostics always describe one population.
  */
  auto lock = std::lock_guard(control->mutex);
  TINYDB_CHECK(control->active_readers != 0, "reader gate active count underflow");
  if (previous != nullptr) {
    previous->next = next;
  } else {
    TINYDB_CHECK(control->oldest_reader == this, "reader gate lost its oldest reader");
    control->oldest_reader = next;
  }
  if (next != nullptr) {
    next->previous = previous;
  } else {
    TINYDB_CHECK(control->newest_reader == this, "reader gate lost its newest reader");
    control->newest_reader = previous;
  }
  --control->active_readers;
  if (control->active_readers == 0) {
    control->changed.notify_all();
  }
}

namespace {

void Admit(std::shared_ptr<ReaderGateControl> control, ReaderGateAdmission *admission) {
  TINYDB_CHECK(control != nullptr, "reader gate received a null control");
  TINYDB_CHECK(admission != nullptr, "reader gate received a null admission");
  TINYDB_CHECK(!admission->admitted, "reader gate received an active admission");
  admission->control = std::move(control);
  auto *const gate = admission->control.get();

  auto lock = std::unique_lock(gate->mutex);
  gate->changed.wait(lock, [gate] { return !gate->publication_pending; });

  // Capture the state and join the active set in one critical section. A
  // publisher can therefore order this reader wholly before or after itself.
  admission->state = gate->state;
  admission->started_at = Clock::now();
  admission->previous = gate->newest_reader;
  if (gate->newest_reader != nullptr) {
    gate->newest_reader->next = admission;
  } else {
    gate->oldest_reader = admission;
  }
  gate->newest_reader = admission;
  ++gate->active_readers;
  admission->admitted = true;
}

}  // namespace

auto SnapshotToken::State() const -> const DatabaseState & {
  TINYDB_CHECK(lease_ != nullptr, "reading an empty snapshot token");
  return *lease_->admission.state;
}

auto ScopedSnapshotToken::State() const -> const DatabaseState & {
  TINYDB_CHECK(admission_.admitted, "reading an empty scoped snapshot token");
  return *admission_.state;
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
  Admit(control_, &lease->admission);
  return SnapshotToken(std::move(lease));
}

void ReaderGate::BeginRead(ScopedSnapshotToken &snapshot) { Admit(control_, &snapshot.admission_); }

auto ReaderGate::BeginPublication() noexcept -> PublicationGuard { return PublicationGuard(control_); }

auto ReaderGate::CurrentState() const -> std::shared_ptr<const DatabaseState> {
  auto lock = std::lock_guard(control_->mutex);
  return control_->state;
}

void ReaderGate::AdvanceCheckpoint(std::uint64_t checkpoint_lsn) {
  /*
  ** CHECKPOINT FRONTIER PUBLICATION
  **
  ** The database file is already durable through checkpoint_lsn. The caller's
  ** writer permit excludes transaction publication, while old readers may
  ** safely retain the older immutable state.
  */
  auto lock = std::lock_guard(control_->mutex);
  const auto &current = control_->state;
  TINYDB_CHECK(checkpoint_lsn >= current->checkpoint_lsn, "visible checkpoint frontier moved backward");
  TINYDB_CHECK(checkpoint_lsn <= current->visible_lsn, "checkpoint advanced beyond visible state");
  if (checkpoint_lsn != current->checkpoint_lsn) {
    auto next = std::make_shared<DatabaseState>(*current);
    next->checkpoint_lsn = checkpoint_lsn;
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
  if (control_->oldest_reader != nullptr) {
    result.oldest_reader_age = Clock::now() - control_->oldest_reader->started_at;
  }
  return result;
}

PublicationGuard::PublicationGuard(std::shared_ptr<ReaderGateControl> control) noexcept : control_(std::move(control)) {
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

void PublicationGuard::Publish(std::shared_ptr<const DatabaseState> state) noexcept {
  TINYDB_CHECK(control_ != nullptr, "publishing through an empty publication guard");
  TINYDB_CHECK(state != nullptr, "publishing a null database state");
  auto lock = std::lock_guard(control_->mutex);
  TINYDB_CHECK(control_->publication_pending, "publishing while reader admission is open");
  TINYDB_CHECK(control_->active_readers == 0, "publishing while readers remain active");
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
  control_.reset();
}

}  // namespace tinydb::txn
