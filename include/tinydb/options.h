#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tinydb {

/*
** PageIoMode selects the transport for database pages; it does not change the
** persistent format, locking protocol, or buffered WAL. Direct mode requires
** aligned O_DIRECT transfers and fails when the host cannot provide them.
** Buffered mode uses the kernel page cache and remains the default.
*/
enum class PageIoMode {
  Buffered,
  Direct,
};

/*
** CHECKPOINT PRESSURE POLICY
**
** The soft thresholds request a checkpoint before the next writer begins.
** The hard thresholds apply write backpressure only after repeated checkpoint
** failure. These settings affect memory, log growth, and recovery time; they
** never weaken synchronous commit durability.
*/
struct CheckpointOptions final {
  std::uint64_t wal_trigger_bytes{1U << 20U};
  std::size_t dirty_trigger_bytes{128U << 10U};
  std::uint64_t hard_wal_bytes{4U << 20U};
  std::size_t hard_dirty_bytes{16U << 20U};
  std::size_t failures_before_backpressure{2};
  std::chrono::milliseconds maximum_age{std::chrono::seconds(30)};
};

/*
** Verification records at most max_issues findings. Reaching this limit makes
** the report incomplete because later findings may be omitted. Traversal also
** stops at the first point where continuing would require trusting malformed
** bytes or an unsafe structural edge.
*/
struct VerifyOptions final {
  std::size_t max_issues{64};
};

/*
** OPEN-TIME RESOURCE POLICY
**
** The committed cache target is soft because WAL-durable pages cannot be
** evicted until checkpointed. The write-transaction limit is hard and covers
** private page images plus copied values. A transaction exceeding it aborts
** without changing visible state.
*/
struct Options final {
  std::size_t page_cache_bytes{16U << 20U};
  std::size_t max_write_transaction_bytes{16U << 20U};
  CheckpointOptions checkpoint{};
  PageIoMode page_io_mode{PageIoMode::Buffered};
};

}  // namespace tinydb
