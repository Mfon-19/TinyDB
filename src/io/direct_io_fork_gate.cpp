#include "io/direct_io_fork_gate.h"

#include "util/check.h"

#include <pthread.h>

#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace tinydb::io {
namespace {

enum class RegistrationState {
  Unattempted,
  Succeeded,
  Failed,
};

struct Gate final {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
  RegistrationState registration{RegistrationState::Unattempted};
  int registration_error{0};
  AtForkRegistrar registrar{&pthread_atfork};
  bool fork_pending{false};
  std::size_t active_operations{0};
};

/* pthread_atfork retains its callbacks for process lifetime, so the callback
** state must outlive C++ static destruction. */
auto ProcessGate() -> Gate & {
  static auto *const gate = new Gate{};
  return *gate;
}

void Lock(Gate *gate) noexcept {
  TINYDB_CHECK(pthread_mutex_lock(&gate->mutex) == 0, "could not lock the direct-I/O fork gate");
}

void Unlock(Gate *gate) noexcept {
  TINYDB_CHECK(pthread_mutex_unlock(&gate->mutex) == 0, "could not unlock the direct-I/O fork gate");
}

void PrepareFork() noexcept {
  auto &gate = ProcessGate();
  Lock(&gate);
  gate.fork_pending = true;
  while (gate.active_operations != 0) {
    TINYDB_CHECK(pthread_cond_wait(&gate.condition, &gate.mutex) == 0, "could not drain direct I/O before fork");
  }
  // Keep the mutex locked across fork. The parent unlocks its copy; the child
  // copy remains locked, preventing I/O through inherited handles.
}

void ParentAfterFork() noexcept {
  auto &gate = ProcessGate();
  gate.fork_pending = false;
  TINYDB_CHECK(pthread_cond_broadcast(&gate.condition) == 0, "could not reopen direct I/O after fork");
  Unlock(&gate);
}

auto RegistrationError(int error) -> Status {
  return Status::IoError("pthread_atfork: " + std::generic_category().message(error));
}

}  // namespace
auto EnsureDirectIoForkGate() -> Status {
  auto &gate = ProcessGate();
  auto error = 0;
  Lock(&gate);
  if (gate.registration == RegistrationState::Unattempted) {
    // Registration is process-wide and cannot be repaired after failure. A
    // direct open must fail because private-memory I/O would be unsafe.
    error = gate.registrar(&PrepareFork, &ParentAfterFork, nullptr);
    gate.registration_error = error;
    gate.registration = error == 0 ? RegistrationState::Succeeded : RegistrationState::Failed;
  } else {
    error = gate.registration_error;
  }
  Unlock(&gate);

  return error == 0 ? Status{} : RegistrationError(error);
}

auto DirectIoOperation::Begin() -> Result<DirectIoOperation> {
  if (auto status = EnsureDirectIoForkGate(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  auto &gate = ProcessGate();
  Lock(&gate);
  while (gate.fork_pending) {
    TINYDB_CHECK(pthread_cond_wait(&gate.condition, &gate.mutex) == 0, "could not wait for direct-I/O fork completion");
  }
  if (gate.active_operations == std::numeric_limits<std::size_t>::max()) {
    Unlock(&gate);
    return std::unexpected(Status::ResourceExhausted("direct-I/O operation counter exhausted"));
  }
  ++gate.active_operations;
  Unlock(&gate);
  return DirectIoOperation(true);
}

DirectIoOperation::DirectIoOperation(DirectIoOperation &&other) noexcept
    : active_(std::exchange(other.active_, false)) {}

auto DirectIoOperation::operator=(DirectIoOperation &&other) noexcept -> DirectIoOperation & {
  if (this != &other) {
    Release();
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

DirectIoOperation::~DirectIoOperation() { Release(); }

void DirectIoOperation::Release() noexcept {
  if (!active_) {
    return;
  }
  active_ = false;

  auto &gate = ProcessGate();
  Lock(&gate);
  TINYDB_CHECK(gate.active_operations != 0, "direct-I/O operation counter underflow");
  --gate.active_operations;
  if (gate.active_operations == 0) {
    TINYDB_CHECK(pthread_cond_broadcast(&gate.condition) == 0, "could not signal drained direct I/O");
  }
  Unlock(&gate);
}

void SetAtForkRegistrarForTest(AtForkRegistrar registrar) {
  TINYDB_CHECK(registrar != nullptr, "installing a null atfork registrar");
  auto &gate = ProcessGate();
  Lock(&gate);
  TINYDB_CHECK(gate.registration == RegistrationState::Unattempted, "changing the atfork registrar after gate use");
  TINYDB_CHECK(!gate.fork_pending, "changing the atfork registrar during fork");
  TINYDB_CHECK(gate.active_operations == 0, "changing the atfork registrar during direct I/O");
  gate.registrar = registrar;
  Unlock(&gate);
}

auto DirectIoForkGateSnapshotForTest() -> DirectIoForkGateSnapshot {
  auto &gate = ProcessGate();
  Lock(&gate);
  const auto snapshot = DirectIoForkGateSnapshot{
      .registered = gate.registration == RegistrationState::Succeeded,
      .fork_pending = gate.fork_pending,
      .active_operations = gate.active_operations,
  };
  Unlock(&gate);
  return snapshot;
}

void SetDirectIoActiveOperationsForTest(std::size_t active_operations) {
  auto &gate = ProcessGate();
  Lock(&gate);
  TINYDB_CHECK(!gate.fork_pending, "changing direct-I/O operations during fork");
  gate.active_operations = active_operations;
  Unlock(&gate);
}

}  // namespace tinydb::io
