#pragma once

#include <tinydb/page.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb {

// A leaf node decoded into sorted key/value records. Mutations edit the
// records in memory; Store rewrites the whole page fully packed. The class
// owns the sortedness invariant: every mutation keeps records ordered by key.
class LeafNode {
 public:
  struct Record {
    std::string key;
    std::string value;
  };

  struct SplitResult;  // defined below; it holds a complete LeafNode

  LeafNode() = default;

  // Decodes a leaf page, verifying node type, cell bounds, and key order.
  // Tombstoned cells are skipped, so they vanish on the next Store.
  static auto Load(const char *page) -> LeafNode;

  // Rewrites the page fully packed: slots ascend after the header, cells
  // grow down from the end of the page.
  void Store(char *page) const;

  // Inserts key or replaces its existing value. Returns true iff the record
  // landed past the current last key (a tail insert; used to spot
  // sequential loads).
  auto Upsert(std::string_view key, std::string_view value) -> bool;

  // Removes key. Returns false if it was not present.
  auto Erase(std::string_view key) -> bool;

  auto Get(std::string_view key) const -> std::optional<std::string>;

  auto Fits() const -> bool;
  auto Underfull() const -> bool;

  // Splits this overflowing node in two: this node keeps the left half and
  // chains to right_page_id, the returned right half keeps the old next
  // leaf. tail_heavy splits "everything old | just the new record" so a
  // sequential load leaves near-full leaves behind.
  auto Split(page_id_t right_page_id, bool tail_heavy) -> SplitResult;

  // Merges the right sibling in: appends its records and takes over its
  // next-leaf link.
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
  std::string separator;  // first key of the right half; the parent copies it
};

}  // namespace tinydb
