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

constexpr std::size_t MAX_ENTRY_BYTES = PAGE_SIZE / 4 - 32;

class BPlusTree {
 public:
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
  page_id_t root_page_id_;
};
}  // namespace tinydb
