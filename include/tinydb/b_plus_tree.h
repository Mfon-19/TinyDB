#pragma once

#include <tinydb/buffer_pool.h>
#include <tinydb/status.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {

// Temporary page-derived cap: it guarantees that any overflowing node has a
// legal byte-balanced split. The long-term value contract is independent of
// page geometry and will use overflow pages.
constexpr std::size_t MAX_ENTRY_BYTES = PAGE_SIZE / 4 - 32;

// Ordered unique-key map over slotted leaf and internal pages. Internal keys
// are lower bounds for their right child; leaves form a forward chain for
// range scans. This implementation still edits through BufferPool directly;
// Milestone 3 introduces a page-source boundary without changing these map
// semantics.
class BPlusTree {
 public:
  // Attaches to an existing root, or initializes a newly allocated all-zero
  // root as an empty leaf.
  static auto Open(BufferPool *buffer_pool, page_id_t root_page_id) -> Result<BPlusTree>;

  auto Put(std::string_view key, std::string_view value) -> Status;
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Remove(std::string_view key) -> Status;
  auto Scan(std::string_view start, std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>>;

  // Walks the complete tree and checks cross-page invariants that normal
  // point operations cannot see: routing ranges, duplicate/cyclic page
  // references, the leaf chain, and allocation/free-list accounting.
  auto CheckIntegrity(page_id_t next_page_id, const std::unordered_set<page_id_t> &free_pages) -> Status;

 private:
  BPlusTree(BufferPool *buffer_pool, page_id_t root_page_id) : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

  BufferPool *buffer_pool_;

  // The current implementation preserves a permanent root page ID by copying
  // split/collapsed root contents. Milestone 3 moves root identity into the
  // transaction state so the ID may change naturally.
  page_id_t root_page_id_;
};
}  // namespace tinydb
