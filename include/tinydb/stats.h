#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tinydb {

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

  std::size_t active_readers{0};
  bool publication_pending{false};
  std::optional<std::chrono::milliseconds> oldest_reader_age;

  std::size_t cache_target_bytes{0};
  std::size_t cache_resident_bytes{0};
  std::size_t cache_resident_pages{0};
  std::size_t cache_pinned_pages{0};
  std::size_t dirty_pages{0};

  std::size_t consecutive_checkpoint_failures{0};
  bool checkpoint_requested{false};
};

}  // namespace tinydb
