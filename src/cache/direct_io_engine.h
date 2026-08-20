#pragma once

#include <tinydb/status.h>

#include "cache/page_arena.h"
#include "storage/page.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace tinydb {

class DiskManager;

namespace cache {

struct DirectIoCheckpointPage final {
  page_id_t page_id;
  const char *data;
};

enum class DirectReadRunState {
  Queued,
  Loading,
  Ready,
  Failed,
  Cancelled,
};

/*
** NONBLOCKING EXACT-PAGE READ
**
** The native reactor uses this object for planned direct-I/O read-ahead. The
** run owns its arena leases until the caller takes a ready page or the run
** releases it. Cancellation is logical and does not submit a kernel
** cancellation request; a loading run releases its leases only after every
** submitted operation completes. The run may safely outlive its engine.
*/
class DirectReadRun final {
 public:
  struct Impl;

  DirectReadRun(const DirectReadRun &) = delete;
  auto operator=(const DirectReadRun &) -> DirectReadRun & = delete;
  ~DirectReadRun();

  auto State() const noexcept -> DirectReadRunState;
  auto Wait() noexcept -> DirectReadRunState;
  auto TakePage(std::size_t index) noexcept -> PageArena::Lease;
  void Cancel() noexcept;

 private:
  explicit DirectReadRun(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

  std::shared_ptr<Impl> impl_;

  friend class DirectIoEngine;
};

/*
** NATIVE DIRECT-I/O ENGINE
**
** A direct page cache constructs this engine and uses it only if io_uring
** initializes successfully; a buffered cache never constructs it. When
** available, one reactor owns the ring used for exact read-ahead and
** checkpoint writes. Exact scheduling publishes every page in a run as one
** unit, while checkpoint writes wait for completion. A null read result means
** that no usable run was handed to the caller; any run scheduled before a
** wrapper-allocation failure has already been drained. A false checkpoint
** result guarantees that no native checkpoint transfer started, so the caller
** may use synchronous direct I/O. Failure never changes the selected
** page-file transport.
**
** The engine is a physical batching layer, not a cache or visibility layer.
** The cache chooses exact page IDs, owns cache admission and LRU policy, and
** validates bytes after completion. Keeping those decisions above the reactor
** prevents CQE order from becoming semantic traversal or publication order.
*/
class DirectIoEngine final {
 public:
  using CompletionHookForTest = void (*)(void *context, bool writing, page_id_t first_page_id, std::size_t page_count,
                                         int *completion_result) noexcept;

  explicit DirectIoEngine(DiskManager *disk);
  DirectIoEngine(const DirectIoEngine &) = delete;
  auto operator=(const DirectIoEngine &) -> DirectIoEngine & = delete;
  ~DirectIoEngine();

  auto ScheduleExact(std::vector<page_id_t> page_ids,
                     std::vector<PageArena::Lease> pages) noexcept -> std::shared_ptr<DirectReadRun>;
  auto WriteCheckpointPages(std::span<const DirectIoCheckpointPage> pages,
                            page_id_t captured_logical_page_count) -> Result<bool>;

  auto Available() const noexcept -> bool;
  void DrainForTesting();
  void SetCompletionHookForTest(CompletionHookForTest hook, void *context) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cache
}  // namespace tinydb
