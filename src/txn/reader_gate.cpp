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
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<const DatabaseState> state;
  bool publication_pending{false};

  // Admissions append in clock order. Intrusive links live either in the
  // shared cursor lease or in a point read's stack token, avoiding a separate
  // diagnostic allocation.
  ReaderGateAdmission *oldest_reader{nullptr};
  ReaderGateAdmission *newest_reader{nullptr};
  std::size_t active_readers{0};
};

ReaderGateAdmission::~ReaderGateAdmission() {
  if (control == nullptr) {
    return;
  }

  // Unlink and decrement under the same mutex, so active count and oldest-age
  // diagnostics always describe the same admitted population.
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
  TINYDB_CHECK(admission->control == nullptr, "reader gate received an active admission");
  auto *const gate = control.get();

  auto lock = std::unique_lock(gate->mutex);
  gate->changed.wait(lock, [gate] { return !gate->publication_pending; });

  // Capture the state and join the active set in one critical section. A
  // publisher can therefore order this reader wholly before or after itself.
  admission->control = std::move(control);
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
}

}  // namespace
auto SnapshotToken::State() const -> const DatabaseState & {
  TINYDB_CHECK(admission_ != nullptr, "reading an empty snapshot token");
  return *admission_->state;
}

auto ScopedSnapshotToken::State() const -> const DatabaseState & {
  TINYDB_CHECK(admission_.control != nullptr, "reading an empty scoped snapshot token");
  return *admission_.state;
}

ReaderGate::ReaderGate(std::shared_ptr<const DatabaseState> initial_state)
    : control_(std::make_shared<ReaderGateControl>()) {
  TINYDB_CHECK(initial_state != nullptr, "reader gate requires an initial database state");
  control_->state = std::move(initial_state);
}

auto ReaderGate::BeginRead() -> SnapshotToken {
  // Allocate the shared admission before it joins the gate. If allocation
  // fails, the reader count has not changed and needs no rollback.
  auto admission = std::make_shared<ReaderGateAdmission>();
  Admit(control_, admission.get());
  return SnapshotToken(std::move(admission));
}

void ReaderGate::BeginRead(ScopedSnapshotToken &snapshot) { Admit(control_, &snapshot.admission_); }

auto ReaderGate::BeginPublication() noexcept -> PublicationGuard { return PublicationGuard(control_); }

auto ReaderGate::CurrentState() const -> std::shared_ptr<const DatabaseState> {
  auto lock = std::lock_guard(control_->mutex);
  return control_->state;
}

void ReaderGate::AdvanceCheckpoint(std::uint64_t checkpoint_lsn) {
  // The file is already durable through checkpoint_lsn. The writer permit
  // excludes transaction publication, while old readers may retain their
  // prior immutable DatabaseState.
  auto lock = std::lock_guard(control_->mutex);
  const auto &current = control_->state;
  TINYDB_CHECK(checkpoint_lsn >= current->checkpoint_lsn, "visible checkpoint LSN moved backward");
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

  // DatabaseCore's writer permit serializes publishers. Marking pending before
  // waiting is the fairness boundary: new readers cannot prolong this wait.
  control_->publication_pending = true;
  control_->changed.wait(lock, [this] { return control_->active_readers == 0; });
}

PublicationGuard::~PublicationGuard() {
  {
    auto lock = std::lock_guard(control_->mutex);
    control_->publication_pending = false;
  }
  control_->changed.notify_all();
}

void PublicationGuard::Publish(std::shared_ptr<const DatabaseState> state) noexcept {
  TINYDB_CHECK(state != nullptr, "publishing a null database state");
  auto lock = std::lock_guard(control_->mutex);
  TINYDB_CHECK(control_->publication_pending, "publishing while reader admission is open");
  TINYDB_CHECK(control_->active_readers == 0, "publishing while readers remain active");
  // This pointer replacement is the visibility event. No admitted reader can
  // observe it halfway through because the active reader population is empty.
  control_->state = std::move(state);
}

}  // namespace tinydb::txn
