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
** The run owns its arena leases until the caller takes a ready page or the
** run releases it. Cancellation is logical and does not submit a kernel
** cancellation request. A loading run releases its leases only after all
** submitted I/O completes. The run can safely outlive its engine.
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
** One reactor owns one io_uring instance for exact reads and checkpoint
** writes. Exact scheduling publishes all pages in one run as one unit.
** Checkpoint writes wait for completion. A null run or false write result
** means that the caller must use synchronous direct I/O. The engine does not
** silently change the database transport.
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
