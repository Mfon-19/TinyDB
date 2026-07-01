#include <tinydb/b_plus_tree.h>
#include <tinydb/check.h>
#include <tinydb/page_ref.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "node.h"

/**
  B+ tree algorithms over the node codec in node.h.

  Design notes:

  - Every mutation loads the whole node into a sorted vector of records,
    edits the vector, and rewrites the page fully packed. Pages are 4 KiB, so
    the rewrite is cheap, and it buys a lot: no fragmentation, no tombstone
    bookkeeping, no in-place fast paths to keep consistent with a slow path.
    (Cells with flags != 0 are treated as tombstones: skipped on load and
    dropped on the next rewrite.)

  - Nothing fails silently: impossible states abort via TINYDB_CHECK, and no
    structural change is written until everything it depends on is known to
    fit. Entries are capped at MAX_ENTRY_BYTES, which guarantees every
    overflowing node has a valid split point (see node.h). The one deliberate
    soft spot: if a rebalance would promote a separator too fat for the
    parent, the repair is skipped and the child stays underfull — the tree
    remains correct, only occupancy suffers.

  - The root page id never changes. A root split moves both halves to new
    pages and rewrites the root as an internal node; a root collapse copies
    the last child over the root page.

  - Orphaned pages (after merges and collapses) leak until free-page reuse
    exists in the disk manager. Scan's end bound is exclusive.
*/

namespace tinydb {
namespace {

// Index of the child to descend into: 0 routes to first_child, index i > 0
// routes to records[i - 1].right_child (keys >= that separator).
auto FindChildIndex(const Node<InternalLayout> &node, std::string_view key)
    -> std::size_t {
  const auto it = std::upper_bound(
      node.records.begin(), node.records.end(), key,
      [](std::string_view target, const InternalRecord &record) {
        return target < record.key;
      });
  return static_cast<std::size_t>(it - node.records.begin());
}

auto ChildAt(const Node<InternalLayout> &node, std::size_t child_index)
    -> page_id_t {
  return child_index == 0 ? node.link
                          : node.records[child_index - 1].right_child;
}

struct PathStep {
  page_id_t page_id;
  std::size_t child_index;  // this node's position under its parent
};

auto DescendToLeaf(BufferPool *pool, page_id_t root_page_id,
                   std::string_view key) -> std::vector<PathStep> {
  auto path = std::vector<PathStep>{{root_page_id, 0}};
  for (;;) {
    TINYDB_CHECK(path.size() <= 64, "tree too deep; page cycle likely");
    PageRef page(pool, path.back().page_id);
    const auto type = ReadAs<NodeHeader>(page.Data()).type;
    if (type == NodeType::Leaf) {
      return path;
    }
    TINYDB_CHECK(type == NodeType::Internal, "descended into a non-tree page");
    const auto node = LoadNode<InternalLayout>(page.Data());
    const auto child_index = FindChildIndex(node, key);
    path.push_back({ChildAt(node, child_index), child_index});
  }
}

template <typename Layout>
struct SplitResult {
  Node<Layout> left;
  Node<Layout> right;
  std::string separator;
};

// tail_heavy: a sequential load inserting at the tail of the rightmost leaf
// splits "everything old | just the new record" instead of down the middle,
// so ascending inserts leave near-full leaves behind.
template <typename Layout>
auto SplitRecords(const Node<Layout> &node, page_id_t right_page_id,
                  bool tail_heavy) -> SplitResult<Layout> {
  constexpr bool is_internal = Layout::NODE_TYPE == NodeType::Internal;
  const bool lopsided = tail_heavy && !is_internal && node.records.size() >= 2;
  const std::size_t split = lopsided ? node.records.size() - 1
                                     : ChooseSplitIndex<Layout>(node.records);
  const auto split_it =
      node.records.begin() + static_cast<std::ptrdiff_t>(split);

  SplitResult<Layout> result;
  result.separator = node.records[split].key;
  result.left.records.assign(node.records.begin(), split_it);
  if constexpr (is_internal) {
    result.left.link = node.link;  // keeps the original first_child
    result.right.link = node.records[split].right_child;
    result.right.records.assign(split_it + 1, node.records.end());
  } else {
    result.left.link = right_page_id;  // splice into the leaf chain
    result.right.link = node.link;
    result.right.records.assign(split_it, node.records.end());
  }
  return result;
}

struct PendingSeparator {
  std::string key;
  page_id_t right_child;
};

// Writes out the two halves of an overflowing node. For a non-root node the
// left half reuses the node's page and the separator is returned for the
// caller to insert into the parent. For the root, both halves move to new
// pages and the root page is rewritten as an internal node above them, so
// the root page id never changes.
template <typename Layout>
auto SplitAndWrite(BufferPool *pool, PageRef &page, const Node<Layout> &node,
                   bool is_root, bool tail_heavy)
    -> std::optional<PendingSeparator> {
  PageRef right_page = PageRef::New(pool);
  const auto split = SplitRecords<Layout>(node, right_page.Id(), tail_heavy);
  StoreNode(right_page.Data(), split.right);
  right_page.MarkDirty();

  if (!is_root) {
    StoreNode(page.Data(), split.left);
    page.MarkDirty();
    return PendingSeparator{split.separator, right_page.Id()};
  }

  PageRef left_page = PageRef::New(pool);
  StoreNode(left_page.Data(), split.left);
  left_page.MarkDirty();

  Node<InternalLayout> new_root;
  new_root.link = left_page.Id();
  new_root.records.push_back(InternalRecord{split.separator, right_page.Id()});
  StoreNode(page.Data(), new_root);
  page.MarkDirty();
  return std::nullopt;
}

// Repairs an underfull child by combining it with a sibling (prefer the one
// to its left) into a single logical node: if that fits in one page the pair
// merges into the left page; otherwise the records are redistributed evenly
// and the parent separator is updated. Returns true iff the pair merged
// (i.e. the parent lost a separator and may itself be underfull now).
template <typename Layout>
auto RepairChild(BufferPool *pool, page_id_t parent_id, const PathStep &child)
    -> bool {
  PageRef parent_page(pool, parent_id);
  auto parent = LoadNode<InternalLayout>(parent_page.Data());
  if (parent.records.empty()) {
    return false;  // an only child has no sibling to repair against
  }

  const std::size_t sep_index =
      child.child_index > 0 ? child.child_index - 1 : 0;
  const page_id_t left_id = ChildAt(parent, sep_index);
  const page_id_t right_id = parent.records[sep_index].right_child;

  PageRef left_page(pool, left_id);
  PageRef right_page(pool, right_id);
  auto left = LoadNode<Layout>(left_page.Data());
  auto right = LoadNode<Layout>(right_page.Data());

  Node<Layout> combined;
  combined.records = std::move(left.records);
  if constexpr (Layout::NODE_TYPE == NodeType::Internal) {
    combined.link = left.link;
    // The parent separator comes down between the halves; the right node's
    // first_child becomes its right_child.
    combined.records.push_back(
        InternalRecord{parent.records[sep_index].key, right.link});
  } else {
    combined.link = right.link;
  }
  combined.records.insert(combined.records.end(),
                          std::make_move_iterator(right.records.begin()),
                          std::make_move_iterator(right.records.end()));

  if (NodeFits<Layout>(combined.records)) {
    // Merge into the left page; the right page is orphaned until free-page
    // reuse exists.
    StoreNode(left_page.Data(), combined);
    left_page.MarkDirty();
    parent.records.erase(parent.records.begin() +
                         static_cast<std::ptrdiff_t>(sep_index));
    StoreNode(parent_page.Data(), parent);
    parent_page.MarkDirty();
    return true;
  }

  // Rebalance: redistribute evenly (this is exactly a split of the combined
  // node) and promote the new separator into the parent.
  const auto split = SplitRecords<Layout>(combined, right_id, false);
  parent.records[sep_index].key = split.separator;
  if (!NodeFits<InternalLayout>(parent.records)) {
    // The fatter separator does not fit in the parent. Skip the repair
    // before touching anything: the child stays underfull but the tree
    // stays correct.
    return false;
  }
  StoreNode(left_page.Data(), split.left);
  left_page.MarkDirty();
  StoreNode(right_page.Data(), split.right);
  right_page.MarkDirty();
  StoreNode(parent_page.Data(), parent);
  parent_page.MarkDirty();
  return false;
}

// While the root is an internal node with no separators, its single child is
// the whole tree: copy the child over the root page (stable root page id).
void CollapseRoot(BufferPool *pool, page_id_t root_page_id) {
  for (;;) {
    PageRef root_page(pool, root_page_id);
    if (ReadAs<NodeHeader>(root_page.Data()).type != NodeType::Internal) {
      return;
    }
    const auto root = LoadNode<InternalLayout>(root_page.Data());
    if (!root.records.empty()) {
      return;
    }
    PageRef child_page(pool, root.link);
    std::memcpy(root_page.Data(), child_page.Data(), PAGE_SIZE);
    root_page.MarkDirty();
  }
}

}  // namespace

BPlusTree::BPlusTree(BufferPool *buffer_pool, page_id_t root_page_id)
    : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {
  TINYDB_CHECK(buffer_pool_ != nullptr, "buffer pool is null");
  TINYDB_CHECK(root_page_id_ != HEADER_PAGE_ID,
               "root page id is the reserved header page");

  // A freshly allocated page is all zeroes; bootstrap it as an empty leaf.
  // NodeType has no zero enumerator, so inspect the raw bytes.
  PageRef root_page(buffer_pool_, root_page_id_);
  const auto raw_type = ReadAs<std::uint16_t>(root_page.Data());
  if (raw_type == 0) {
    StoreNode(root_page.Data(), Node<LeafLayout>{});
    root_page.MarkDirty();
    return;
  }
  const bool is_node =
      raw_type == static_cast<std::uint16_t>(NodeType::Leaf) ||
      raw_type == static_cast<std::uint16_t>(NodeType::Internal);
  TINYDB_CHECK(is_node, "root page is not a b+ tree node");
}

void BPlusTree::Put(std::string_view key, std::string_view value) {
  TINYDB_CHECK(key.size() + value.size() <= MAX_ENTRY_BYTES,
               "entry exceeds MAX_ENTRY_BYTES; enforce sizes before Put");
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);

  std::optional<PendingSeparator> pending;
  {
    PageRef leaf_page(buffer_pool_, path.back().page_id);
    auto node = LoadNode<LeafLayout>(leaf_page.Data());
    const auto it = LowerBoundByKey(node.records, key);
    const bool at_tail = it == node.records.end();
    if (!at_tail && it->key == key) {
      it->value.assign(value.data(), value.size());
    } else {
      node.records.insert(it,
                          LeafRecord{std::string(key), std::string(value)});
    }

    if (NodeFits<LeafLayout>(node.records)) {
      StoreNode(leaf_page.Data(), node);
      leaf_page.MarkDirty();
      return;
    }
    const bool tail_heavy = at_tail && node.link == HEADER_PAGE_ID;
    pending = SplitAndWrite(buffer_pool_, leaf_page, node,
                            /*is_root=*/path.size() == 1, tail_heavy);
  }

  // Insert the pending separator into the ancestors, splitting as needed.
  std::size_t level = path.size() - 1;
  while (pending.has_value()) {
    TINYDB_CHECK(level > 0, "pending separator escaped the root");
    --level;
    PageRef page(buffer_pool_, path[level].page_id);
    auto node = LoadNode<InternalLayout>(page.Data());
    const auto it = LowerBoundByKey(node.records, pending->key);
    node.records.insert(
        it, InternalRecord{std::move(pending->key), pending->right_child});

    if (NodeFits<InternalLayout>(node.records)) {
      StoreNode(page.Data(), node);
      page.MarkDirty();
      return;
    }
    pending = SplitAndWrite(buffer_pool_, page, node,
                            /*is_root=*/level == 0, /*tail_heavy=*/false);
  }
}

auto BPlusTree::Get(std::string_view key) -> std::optional<std::string> {
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);
  PageRef leaf_page(buffer_pool_, path.back().page_id);
  auto node = LoadNode<LeafLayout>(leaf_page.Data());
  const auto it = LowerBoundByKey(node.records, key);
  if (it != node.records.end() && it->key == key) {
    return std::move(it->value);
  }
  return std::nullopt;
}

void BPlusTree::Remove(std::string_view key) {
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);
  {
    PageRef leaf_page(buffer_pool_, path.back().page_id);
    auto node = LoadNode<LeafLayout>(leaf_page.Data());
    const auto it = LowerBoundByKey(node.records, key);
    if (it == node.records.end() || it->key != key) {
      return;
    }
    node.records.erase(it);
    StoreNode(leaf_page.Data(), node);
    leaf_page.MarkDirty();

    const bool is_root_leaf = path.size() == 1;
    if (is_root_leaf ||
        NodeBytes<LeafLayout>(node.records) >= MIN_FILL_BYTES<LeafLayout>) {
      return;
    }
  }

  // Repair underfull nodes bottom-up. A rebalance (or a skipped repair) ends
  // the walk; a merge shrinks the parent, which may need repair itself.
  bool leaf_level = true;
  for (std::size_t level = path.size() - 1; level > 0; --level) {
    const bool merged =
        leaf_level
            ? RepairChild<LeafLayout>(buffer_pool_, path[level - 1].page_id,
                                      path[level])
            : RepairChild<InternalLayout>(buffer_pool_,
                                          path[level - 1].page_id, path[level]);
    if (!merged) {
      return;
    }
    leaf_level = false;

    if (level - 1 == 0) {
      break;  // the parent is the root; handled by CollapseRoot below
    }
    PageRef parent_page(buffer_pool_, path[level - 1].page_id);
    const auto parent = LoadNode<InternalLayout>(parent_page.Data());
    if (NodeBytes<InternalLayout>(parent.records) >=
        MIN_FILL_BYTES<InternalLayout>) {
      return;
    }
  }
  CollapseRoot(buffer_pool_, root_page_id_);
}

auto BPlusTree::Scan(std::string_view start, std::string_view end)
    -> std::vector<std::pair<std::string, std::string>> {
  auto rows = std::vector<std::pair<std::string, std::string>>{};
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, start);

  auto page_id = path.back().page_id;
  while (page_id != HEADER_PAGE_ID) {
    PageRef leaf_page(buffer_pool_, page_id);
    auto node = LoadNode<LeafLayout>(leaf_page.Data());
    for (auto it = LowerBoundByKey(node.records, start);
         it != node.records.end(); ++it) {
      if (it->key >= end) {
        return rows;
      }
      rows.emplace_back(std::move(it->key), std::move(it->value));
    }
    page_id = node.link;
  }
  return rows;
}

}  // namespace tinydb
