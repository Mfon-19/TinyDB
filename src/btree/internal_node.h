#pragma once

#include <tinydb/page.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb {

// An internal node decoded into a first child plus sorted separator records.
// A node with N separators has N + 1 children: separator i routes keys >= its
// key into records[i].right_child, keys below every separator go to the
// first child. Mutations edit the records in memory; Store rewrites the
// whole page fully packed. The class owns the sortedness invariant.
class InternalNode {
 public:
  struct Record {
    std::string key;        // separator
    page_id_t right_child;  // subtree holding keys >= key
  };

  struct SplitResult;  // defined below; it holds a complete InternalNode

  InternalNode() = default;

  // A fresh root above the two halves of a root split.
  InternalNode(page_id_t first_child, std::string separator, page_id_t right_child);

  // Decodes an internal page, verifying node type, cell bounds, and key
  // order.
  static auto Load(const char *page) -> InternalNode;

  // Rewrites the page fully packed: slots ascend after the header, cells
  // grow down from the end of the page.
  void Store(char *page) const;

  // Index of the child to descend into for key: 0 routes to the first
  // child, index i > 0 routes to separator i - 1's right child.
  auto FindChildIndex(std::string_view key) const -> std::size_t;
  auto ChildAt(std::size_t child_index) const -> page_id_t;

  auto SeparatorCount() const -> std::size_t { return records_.size(); }
  auto SeparatorKeyAt(std::size_t index) const -> const std::string &;
  void SetSeparatorKey(std::size_t index, std::string key);
  void InsertSeparator(std::string key, page_id_t right_child);
  void EraseSeparator(std::size_t index);

  auto Fits() const -> bool;
  auto Underfull() const -> bool;

  // Splits this overflowing node in two: this node keeps the left half, the
  // separator between the halves moves up to the parent, and its right
  // child becomes the returned right half's first child.
  auto Split() -> SplitResult;

  // Merges the right sibling in: the pulled-down parent separator sits
  // between the halves, with the right node's first child hanging off it.
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
  std::string separator;  // moves up to the parent
};

}  // namespace tinydb
