#pragma once

#include <tinydb/status.h>

#include <cstddef>

namespace tinydb::io {

using AtForkRegistrar = int (*)(void (*)(), void (*)(), void (*)());

/*
** Linux makes fork unsafe while direct I/O targets private memory. Each
** synchronous transfer and prepared asynchronous request therefore holds one
** DirectIoOperation admission for the complete kernel access. The atfork
** prepare handler closes admission and drains active operations before fork;
** only the parent reopens it. The child must exec or exit without using an
** inherited TinyDB handle. Buffered transfers never enter this gate.
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

/* Registration is process-wide and cannot be reset after the first attempt. */
void SetAtForkRegistrarForTest(AtForkRegistrar registrar);
auto DirectIoForkGateSnapshotForTest() -> DirectIoForkGateSnapshot;
void SetDirectIoActiveOperationsForTest(std::size_t active_operations);

}  // namespace tinydb::io
