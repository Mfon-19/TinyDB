#pragma once

#include <tinydb/status.h>

#include <cstddef>

namespace tinydb::io {

using AtForkRegistrar = int (*)(void (*)(), void (*)(), void (*)());

/*
** Linux makes fork unsafe while private-memory direct I/O is in flight.
** Each synchronous transfer and each prepared asynchronous request holds one
** admission. DirectIoOperation is the admission token. Its lifetime covers
** each kernel access to private memory. The atfork prepare handler stops new
** transfers and drains current transfers before fork. Admission reopens only
** in the parent. The child must exec or exit without using inherited TinyDB
** handles.
*/
class DirectIoOperation final {
 public:
  static auto Begin() -> Result<DirectIoOperation>;

  DirectIoOperation() = default;

  DirectIoOperation(const DirectIoOperation &) = delete;
  auto operator=(const DirectIoOperation &) -> DirectIoOperation & = delete;
  DirectIoOperation(DirectIoOperation &&other) noexcept;
  auto operator=(DirectIoOperation &&other) noexcept -> DirectIoOperation &;
  ~DirectIoOperation();

 private:
  explicit DirectIoOperation(bool active) : active_(active) {}
  void Release() noexcept;

  bool active_{false};
};

auto EnsureDirectIoForkGate() -> Status;

struct DirectIoForkGateSnapshot final {
  bool registered;
  bool fork_pending;
  std::size_t active_operations;
};

// Registration is process-wide and cannot be reset after the first attempt.
void SetAtForkRegistrarForTest(AtForkRegistrar registrar);
auto DirectIoForkGateSnapshotForTest() -> DirectIoForkGateSnapshot;
void SetDirectIoActiveOperationsForTest(std::size_t active_operations);

}  // namespace tinydb::io
