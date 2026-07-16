#pragma once

#include <tinydb/page.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb {

class InternalNode {
 public:
  struct Record {
    std::string key;
    page_id_t right_child;
  };

  struct SplitResult;

  InternalNode() = default;
  InternalNode(page_id_t first_child, std::string separator, page_id_t right_child);

  static auto Load(const char *page) -> InternalNode;
  void Store(char *page, page_id_t page_id) const;

  auto FindChildIndex(std::string_view key) const -> std::size_t;
  auto ChildAt(std::size_t child_index) const -> page_id_t;

  auto SeparatorCount() const -> std::size_t { return records_.size(); }
  auto SeparatorKeyAt(std::size_t index) const -> const std::string &;
  void SetSeparatorKey(std::size_t index, std::string key);
  void InsertSeparator(std::string key, page_id_t right_child);
  void EraseSeparator(std::size_t index);

  auto Fits() const -> bool;
  auto Underfull() const -> bool;

  auto Split() -> SplitResult;
  void Absorb(std::string separator, InternalNode &&right);

  auto FirstChild() const -> page_id_t { return first_child_; }

 private:
  auto Bytes() const -> std::size_t;
  auto ChooseSplitIndex() const -> std::size_t;

  page_id_t first_child_{HEADER_PAGE_ID};
  std::vector<Record> records_;
};

struct InternalNode::SplitResult {
  InternalNode right;
  std::string separator;
};

}  // namespace tinydb
