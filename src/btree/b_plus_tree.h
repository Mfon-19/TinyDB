#pragma once

#include <tinydb/limits.h>
#include <tinydb/page.h>
#include <tinydb/status.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {

class PageSource;
class PageReader;

// Ordered map algorithms over pages supplied by PageSource. This type owns the
// current root id but owns no page memory.
class BPlusTree {
 public:
  // Existing roots are validated; a zero-filled allocated root becomes a leaf.
  static auto Open(PageSource *pages, page_id_t root_page_id) -> Result<BPlusTree>;

  auto Put(std::string_view key, std::string_view value) -> Status;
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Remove(std::string_view key) -> Status;
  auto Scan(std::string_view start, std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>>;

  // The caller publishes this value with the page changes that produced it.
  auto RootPageId() const -> page_id_t { return root_page_id_; }

  // Validates relationships that a single-page decoder cannot see.
  auto CheckIntegrity(page_id_t next_page_id, const std::unordered_set<page_id_t> &free_pages) -> Status;
  static auto CheckIntegrity(PageReader *pages, page_id_t root_page_id, page_id_t next_page_id,
                             const std::unordered_set<page_id_t> &free_pages,
                             const std::unordered_set<page_id_t> &allocator_pages) -> Status;

 private:
  BPlusTree(PageSource *pages, page_id_t root_page_id) : pages_(pages), root_page_id_(root_page_id) {}

  PageSource *pages_;
  page_id_t root_page_id_;
};
}  // namespace tinydb
