#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tinydb {

enum class VerifyIssueKind {
  Page,
  TreeStructure,
  LeafLink,
  OverflowValue,
  Allocator,
  DoubleAllocation,
  LeakedPage,
};

/* A page ID of zero identifies file-wide metadata rather than a data page. */
struct VerifyIssue final {
  VerifyIssueKind kind{VerifyIssueKind::Page};
  std::uint64_t page_id{0};
  std::string message;
};

/*
** Verification reports corruption as data instead of losing the partial audit
** behind an error Status. Result<VerifyReport> is reserved for environmental
** failures such as I/O or memory exhaustion. complete is false when malformed
** bytes made further traversal unsafe.
*/
struct VerifyReport final {
  std::uint64_t transaction_id{0};
  std::uint64_t visible_lsn{0};
  bool complete{true};

  std::size_t pages_checked{0};
  std::size_t leaf_pages{0};
  std::size_t internal_pages{0};
  std::size_t overflow_pages{0};
  std::size_t allocator_pages{0};
  std::size_t reusable_pages{0};
  std::size_t retired_pages{0};
  std::vector<VerifyIssue> issues;

  auto Ok() const noexcept -> bool { return complete && issues.empty(); }
};

/*
** A diagnostic snapshot of the currently open handle. Statistics do not
** participate in transaction visibility and never cause storage I/O.
** Dirty pages are WAL-durable page versions not yet represented by the
** checkpointed database file.
*/
struct DatabaseStats final {
  std::uint64_t transaction_id{0};
  std::uint64_t visible_lsn{0};
  std::uint64_t checkpoint_lsn{0};
  std::uint64_t wal_bytes{0};
  std::size_t wal_segments{0};

  std::size_t active_readers{0};
  bool publication_pending{false};
  std::optional<std::chrono::milliseconds> oldest_reader_age;

  std::size_t cache_target_bytes{0};
  std::size_t cache_resident_bytes{0};
  std::size_t cache_resident_pages{0};
  std::size_t cache_pinned_pages{0};
  std::size_t dirty_pages{0};
  std::size_t dirty_bytes{0};
  std::uint64_t cache_hits{0};
  std::uint64_t cache_misses{0};
  std::uint64_t cache_evictions{0};

  std::uint64_t write_attempts{0};
  std::uint64_t committed_writes{0};
  std::chrono::nanoseconds last_write_prepare{};
  std::chrono::nanoseconds last_wal_sync{};
  std::chrono::nanoseconds last_publication_wait{};
  std::chrono::nanoseconds maximum_publication_wait{};

  std::size_t retired_pages{0};
  std::size_t reusable_pages{0};

  std::size_t consecutive_checkpoint_failures{0};
  bool checkpoint_requested{false};
  std::chrono::milliseconds checkpoint_age{};
  std::optional<std::string> last_checkpoint_error;
};

}  // namespace tinydb
