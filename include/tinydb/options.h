#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tinydb {

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

/* Verification stops recording ownership leaks after this many issues. A
** malformed page or unsafe structural edge always stops traversal at the
** first point where continuing would require trusting corrupted bytes. */
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
  std::uint64_t wal_segment_bytes{768U << 10U};
  CheckpointOptions checkpoint{};
};

}  // namespace tinydb
