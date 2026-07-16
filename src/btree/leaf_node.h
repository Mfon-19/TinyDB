#pragma once

#include <tinydb/page.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb {

class LeafNode {
 public:
  struct Record {
    std::string key;
    std::string value;
  };

  struct SplitResult;

  LeafNode() = default;

  static auto Load(const char *page) -> LeafNode;
  void Store(char *page, page_id_t page_id) const;

  auto Upsert(std::string_view key, std::string_view value) -> bool;
  auto Erase(std::string_view key) -> bool;
  auto Get(std::string_view key) const -> std::optional<std::string>;

  auto Fits() const -> bool;
  auto Underfull() const -> bool;

  auto Split(page_id_t right_page_id, bool tail_heavy) -> SplitResult;
  void Absorb(LeafNode &&right);

  auto Records() const -> const std::vector<Record> & { return records_; }
  auto NextLeaf() const -> page_id_t { return next_leaf_; }

 private:
  auto Bytes() const -> std::size_t;
  auto ChooseSplitIndex() const -> std::size_t;

  page_id_t next_leaf_{HEADER_PAGE_ID};
  std::vector<Record> records_;
};

struct LeafNode::SplitResult {
  LeafNode right;
  std::string separator;
};

}  // namespace tinydb
