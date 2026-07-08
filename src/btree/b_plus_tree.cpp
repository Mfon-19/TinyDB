#include <tinydb/b_plus_tree.h>
#include <tinydb/check.h>
#include <tinydb/page_ref.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "internal_node.h"
#include "leaf_node.h"
#include "page_format.h"

/**
  B+ tree algorithms over the LeafNode / InternalNode codecs.

  Design notes:

  - Every mutation loads the whole node into sorted records, edits them, and
    rewrites the page fully packed. Pages are 4 KiB, so the rewrite is cheap,
    and it buys a lot: no fragmentation, no tombstone bookkeeping, no
    in-place fast paths to keep consistent with a slow path. (Cells with
    flags != 0 are treated as tombstones: skipped on load and dropped on the
    next rewrite.)

  - Nothing fails silently: I/O failures propagate out as statuses before
    any dependent page is written, impossible states abort via TINYDB_CHECK,
    and no structural change is written until everything it depends on is
    known to fit. Entries are capped at MAX_ENTRY_BYTES, which guarantees
    every overflowing node has a valid split point (see page_format.h). The
    one deliberate soft spot: if a rebalance would promote a separator too
    fat for the parent, the repair is skipped and the child stays underfull —
    the tree remains correct, only occupancy suffers.

  - The root page id never changes. A root split moves both halves to new
    pages and rewrites the root as an internal node; a root collapse copies
    the last child over the root page.

  - Pages orphaned by merges and root collapses are handed back to the
    buffer pool (BufferPool::FreePage), which drops any cached copy and puts
    them on the disk manager's free list for reuse. Scan's end bound is
    exclusive.

  Logical shape and separator rule:

             internal root
          +-----------------+
          | K10 | K20 | K40 |
          +-----------------+
          /       |     |    \
      < K10   >= K10  >= K20  >= K40
              < K20   < K40

      Internal records are "separator -> right child"; the first child is
      stored separately. Searching uses upper_bound, so a key equal to a
      separator descends to the separator's right child. Leaves hold every
      value and are linked left to right:

          leaf A        leaf B        leaf C
        [a b c]  ---> [k m n]  ---> [x y z]

  Mutation shape:

      Load page bytes -> edit sorted records in memory -> prove the result
      fits or split/repair first -> Store rewrites a fully packed page.

      No structural change is written before the pages it depends on are
      known to fit. When a split creates a separator, that separator is
      propagated upward like a carry bit in addition:

          leaf split produces (separator, right_child)
                    |
                    v
          insert into parent, maybe parent splits too
*/

namespace tinydb {
namespace {

struct PathStep {
  page_id_t page_id;
  std::size_t child_index;  // this node's position under its parent
};

auto DescendToLeaf(BufferPool *pool, page_id_t root_page_id, std::string_view key) -> Result<std::vector<PathStep>> {
  /*
    Walk from the stable root page to the leaf that owns key.

      path[0] = {root_page_id, 0}
      path[1] = {child page, index under root}
      path[2] = {leaf page,  index under parent}

    The child_index values are saved for deletion repair. If the leaf later
    underflows, repair can reopen the parent and know which child position
    points at the underfull node without searching for page ids.
  */
  auto path = std::vector<PathStep>{{root_page_id, 0}};
  for (;;) {
    TINYDB_CHECK(path.size() <= 64, "tree too deep; page cycle likely");
    auto page = PageRef::Fetch(pool, path.back().page_id);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    const auto type = ReadAs<NodeHeader>(page->Data()).type;
    if (type == NodeType::Leaf) {
      return path;
    }
    TINYDB_CHECK(type == NodeType::Internal, "descended into a non-tree page");
    const auto node = InternalNode::Load(page->Data());
    const auto child_index = node.FindChildIndex(key);
    path.push_back({node.ChildAt(child_index), child_index});
  }
}

struct PendingSeparator {
  std::string key;
  page_id_t right_child;
};

// Writes out the two halves of an overflowing leaf. For a non-root leaf the
// left half reuses the leaf's page and the separator is returned for the
// caller to insert into the parent. For the root, both halves move to new
// pages and the root page is rewritten as an internal node above them, so
// the root page id never changes.
auto SplitAndWrite(BufferPool *pool, PageRef &page, LeafNode &node, bool is_root,
                   bool tail_heavy) -> Result<std::optional<PendingSeparator>> {
  /*
    Non-root leaf split:

        before:
          page P: [a b c k m z] -> old_next

        after:
          page P: [a b c] -> page R: [k m z] -> old_next

        return separator:
          (key = "k", right_child = R)

    Root leaf split:

        root page id must stay stable, so the old root page becomes an
        internal page and the two leaf halves move out to fresh pages:

          before: root P is leaf [a ... z]

          after:  root P is internal ["k" -> R]
                   first_child = L

                  L: [a b c] -> R: [k m z]

    `tail_heavy` is the sequential-insert optimization: when the inserted
    key lands at the tail of the rightmost leaf, split as
    "all old records | the new record" so a bulk ascending load leaves
    dense leaves behind.
  */
  auto right_page = PageRef::New(pool);
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split(right_page->Id(), tail_heavy);
  split.right.Store(right_page->Data());
  right_page->MarkDirty();

  if (!is_root) {
    node.Store(page.Data());
    page.MarkDirty();
    return std::optional{PendingSeparator{std::move(split.separator), right_page->Id()}};
  }

  auto left_page = PageRef::New(pool);
  if (!left_page) {
    return std::unexpected(std::move(left_page).error());
  }
  node.Store(left_page->Data());
  left_page->MarkDirty();

  const InternalNode new_root(left_page->Id(), std::move(split.separator), right_page->Id());
  new_root.Store(page.Data());
  page.MarkDirty();
  return std::optional<PendingSeparator>{};
}

// Same as above for an overflowing internal node; no leaf chain or
// tail-heavy concerns here.
auto SplitAndWrite(BufferPool *pool, PageRef &page, InternalNode &node,
                   bool is_root) -> Result<std::optional<PendingSeparator>> {
  /*
    Internal split differs from leaf split in one important way: the
    separator moves up and does not stay in either child.

      before:
        first=C0, records=[K0->C1, K1->C2, K2->C3, K3->C4]

      split at K2:
        left:      first=C0, records=[K0->C1, K1->C2]
        separator: K2
        right:     first=C3, records=[K3->C4]

    For a root split, the root page is again rewritten in place as the new
    internal root and the two halves live on new child pages.
  */
  auto right_page = PageRef::New(pool);
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split();
  split.right.Store(right_page->Data());
  right_page->MarkDirty();

  if (!is_root) {
    node.Store(page.Data());
    page.MarkDirty();
    return std::optional{PendingSeparator{std::move(split.separator), right_page->Id()}};
  }

  auto left_page = PageRef::New(pool);
  if (!left_page) {
    return std::unexpected(std::move(left_page).error());
  }
  node.Store(left_page->Data());
  left_page->MarkDirty();

  const InternalNode new_root(left_page->Id(), std::move(split.separator), right_page->Id());
  new_root.Store(page.Data());
  page.MarkDirty();
  return std::optional<PendingSeparator>{};
}

// Repairs an underfull leaf by combining it with a sibling (prefer the one
// to its left) into a single logical node: if that fits in one page the pair
// merges into the left page; otherwise the records are redistributed evenly
// and the parent separator is updated. Returns true iff the pair merged
// (i.e. the parent lost a separator and may itself be underfull now).
auto RepairLeafChild(BufferPool *pool, page_id_t parent_id, const PathStep &child) -> Result<bool> {
  /*
    Leaf repair always works on adjacent siblings under the same parent.
    If the underfull child has a left sibling, use that pair; otherwise use
    the child and its right sibling.

      parent before:
          ... | sep[i] = first key of R | ...
                    /                 \
              left leaf L          right leaf R

    Combine the two leaves in memory:

          combined = L records + R records

    Case 1: combined fits in one page.

          L := combined
          parent erases sep[i] and the R pointer
          R is freed

      The parent lost one separator, so the caller may have to repair the
      parent too.

    Case 2: combined does not fit.

          split combined back into L and R
          parent sep[i] := first key of new R

      The parent keeps the same child count, so repair stops here. If the
      replacement separator is too fat for the parent, skip the repair:
      the child can remain underfull without breaking search correctness.
  */
  auto parent_page = PageRef::Fetch(pool, parent_id);
  if (!parent_page) {
    return std::unexpected(std::move(parent_page).error());
  }
  auto parent = InternalNode::Load(parent_page->Data());
  if (parent.SeparatorCount() == 0) {
    return false;  // an only child has no sibling to repair against
  }

  const std::size_t sep_index = child.child_index > 0 ? child.child_index - 1 : 0;
  const page_id_t left_id = parent.ChildAt(sep_index);
  const page_id_t right_id = parent.ChildAt(sep_index + 1);

  {
    auto left_page = PageRef::Fetch(pool, left_id);
    if (!left_page) {
      return std::unexpected(std::move(left_page).error());
    }
    auto right_page = PageRef::Fetch(pool, right_id);
    if (!right_page) {
      return std::unexpected(std::move(right_page).error());
    }
    auto combined = LeafNode::Load(left_page->Data());
    combined.Absorb(LeafNode::Load(right_page->Data()));

    if (!combined.Fits()) {
      // Rebalance: redistribute evenly (this is exactly a split of the
      // combined node) and promote the new separator into the parent.
      auto split = combined.Split(right_id, /*tail_heavy=*/false);
      parent.SetSeparatorKey(sep_index, std::move(split.separator));
      if (!parent.Fits()) {
        // The fatter separator does not fit in the parent. Skip the repair
        // before touching anything: the child stays underfull but the tree
        // stays correct.
        return false;
      }
      combined.Store(left_page->Data());
      left_page->MarkDirty();
      split.right.Store(right_page->Data());
      right_page->MarkDirty();
      parent.Store(parent_page->Data());
      parent_page->MarkDirty();
      return false;
    }

    // Merge into the left page.
    combined.Store(left_page->Data());
    left_page->MarkDirty();
    parent.EraseSeparator(sep_index);
    parent.Store(parent_page->Data());
    parent_page->MarkDirty();
  }

  // The merged-away sibling, unpinned above.
  pool->FreePage(right_id);
  return true;
}

// The internal-node version of the repair above. The one difference: the
// parent separator comes down between the two halves when they combine.
auto RepairInternalChild(BufferPool *pool, page_id_t parent_id, const PathStep &child) -> Result<bool> {
  /*
    Internal repair has the same merge-vs-redistribute shape as leaf repair,
    but internal nodes have N separators and N+1 children. The separator in
    the parent is not just a boundary label: it is the key that belongs
    between the two child nodes when they combine.

      parent:
             sep[i]
            /      \
         left      right

      combined logical order:

         left records, sep[i] -> right.first_child, right records

      If combined fits, it replaces the left page and sep[i] disappears
      from the parent. If it does not fit, combined is split again and the
      new middle separator replaces sep[i] in the parent.
  */
  auto parent_page = PageRef::Fetch(pool, parent_id);
  if (!parent_page) {
    return std::unexpected(std::move(parent_page).error());
  }
  auto parent = InternalNode::Load(parent_page->Data());
  if (parent.SeparatorCount() == 0) {
    return false;  // an only child has no sibling to repair against
  }

  const std::size_t sep_index = child.child_index > 0 ? child.child_index - 1 : 0;
  const page_id_t left_id = parent.ChildAt(sep_index);
  const page_id_t right_id = parent.ChildAt(sep_index + 1);

  {
    auto left_page = PageRef::Fetch(pool, left_id);
    if (!left_page) {
      return std::unexpected(std::move(left_page).error());
    }
    auto right_page = PageRef::Fetch(pool, right_id);
    if (!right_page) {
      return std::unexpected(std::move(right_page).error());
    }
    auto combined = InternalNode::Load(left_page->Data());
    combined.Absorb(parent.SeparatorKeyAt(sep_index), InternalNode::Load(right_page->Data()));

    if (!combined.Fits()) {
      auto split = combined.Split();
      parent.SetSeparatorKey(sep_index, std::move(split.separator));
      if (!parent.Fits()) {
        return false;
      }
      combined.Store(left_page->Data());
      left_page->MarkDirty();
      split.right.Store(right_page->Data());
      right_page->MarkDirty();
      parent.Store(parent_page->Data());
      parent_page->MarkDirty();
      return false;
    }

    combined.Store(left_page->Data());
    left_page->MarkDirty();
    parent.EraseSeparator(sep_index);
    parent.Store(parent_page->Data());
    parent_page->MarkDirty();
  }

  // The merged-away sibling, unpinned above.
  pool->FreePage(right_id);
  return true;
}

// While the root is an internal node with no separators, its single child is
// the whole tree: copy the child over the root page (stable root page id).
auto CollapseRoot(BufferPool *pool, page_id_t root_page_id) -> Status {
  /*
    Root collapse preserves the root page id just like root split.

      before:
        root page P: internal, first_child = C, no separators
        child C:     leaf or internal containing the whole tree

      after:
        root page P: byte-for-byte copy of C
        page C:      freed

    The loop handles cascaded collapses, for example when P copies an
    internal child that also has a single child.
  */
  for (;;) {
    page_id_t child_id = HEADER_PAGE_ID;
    {
      auto root_page = PageRef::Fetch(pool, root_page_id);
      if (!root_page) {
        return std::move(root_page).error();
      }
      if (ReadAs<NodeHeader>(root_page->Data()).type != NodeType::Internal) {
        return {};
      }
      const auto root = InternalNode::Load(root_page->Data());
      if (root.SeparatorCount() > 0) {
        return {};
      }
      child_id = root.FirstChild();
      auto child_page = PageRef::Fetch(pool, child_id);
      if (!child_page) {
        return std::move(child_page).error();
      }
      std::memcpy(root_page->Data(), child_page->Data(), PAGE_SIZE);
      root_page->MarkDirty();
    }
    // The swallowed child's page, unpinned above.
    pool->FreePage(child_id);
  }
}

}  // namespace

auto BPlusTree::Open(BufferPool *buffer_pool, page_id_t root_page_id) -> Result<BPlusTree> {
  TINYDB_CHECK(buffer_pool != nullptr, "buffer pool is null");
  TINYDB_CHECK(root_page_id != HEADER_PAGE_ID, "root page id is the reserved header page");

  // A freshly allocated page is all zeroes; bootstrap it as an empty leaf.
  // NodeType has no zero enumerator, so inspect the raw bytes.
  auto root_page = PageRef::Fetch(buffer_pool, root_page_id);
  if (!root_page) {
    return std::unexpected(std::move(root_page).error());
  }
  const auto raw_type = ReadAs<std::uint16_t>(root_page->Data());
  if (raw_type == 0) {
    LeafNode{}.Store(root_page->Data());
    root_page->MarkDirty();
    return BPlusTree(buffer_pool, root_page_id);
  }
  const bool is_node = raw_type == static_cast<std::uint16_t>(NodeType::Leaf) ||
                       raw_type == static_cast<std::uint16_t>(NodeType::Internal);
  if (!is_node) {
    return std::unexpected(Status::Corruption("root page is not a b+ tree node"));
  }
  return BPlusTree(buffer_pool, root_page_id);
}

auto BPlusTree::Put(std::string_view key, std::string_view value) -> Status {
  /*
    Insert/update path:

      1. Descend to the owning leaf.
      2. Upsert in memory.
      3. If the leaf fits, rewrite it and stop.
      4. If it overflows, split it. A non-root split returns a pending
         separator for the parent; a root split builds a new root in place.
      5. Propagate the pending separator upward until some parent absorbs
         it or the root splits.

    The pending separator is the only state carried between levels:

        pending = (separator key, right child page id)
  */
  TINYDB_CHECK(key.size() + value.size() <= MAX_ENTRY_BYTES, "entry exceeds MAX_ENTRY_BYTES; enforce sizes before Put");
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);
  if (!path) {
    return path.error();
  }

  std::optional<PendingSeparator> pending;
  {
    auto leaf_page = PageRef::Fetch(buffer_pool_, path->back().page_id);
    if (!leaf_page) {
      return std::move(leaf_page).error();
    }
    auto node = LeafNode::Load(leaf_page->Data());
    const bool at_tail = node.Upsert(key, value);

    if (node.Fits()) {
      node.Store(leaf_page->Data());
      leaf_page->MarkDirty();
      return {};
    }
    const bool tail_heavy = at_tail && node.NextLeaf() == HEADER_PAGE_ID;
    auto split = SplitAndWrite(buffer_pool_, *leaf_page, node,
                               /*is_root=*/path->size() == 1, tail_heavy);
    if (!split) {
      return std::move(split).error();
    }
    pending = std::move(*split);
  }

  // Insert the pending separator into the ancestors, splitting as needed.
  std::size_t level = path->size() - 1;
  while (pending.has_value()) {
    TINYDB_CHECK(level > 0, "pending separator escaped the root");
    --level;
    auto page = PageRef::Fetch(buffer_pool_, (*path)[level].page_id);
    if (!page) {
      return std::move(page).error();
    }
    auto node = InternalNode::Load(page->Data());
    node.InsertSeparator(std::move(pending->key), pending->right_child);

    if (node.Fits()) {
      node.Store(page->Data());
      page->MarkDirty();
      return {};
    }
    auto split = SplitAndWrite(buffer_pool_, *page, node, /*is_root=*/level == 0);
    if (!split) {
      return std::move(split).error();
    }
    pending = std::move(*split);
  }
  return {};
}

auto BPlusTree::Get(std::string_view key) -> Result<std::optional<std::string>> {
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);
  if (!path) {
    return std::unexpected(path.error());
  }
  auto leaf_page = PageRef::Fetch(buffer_pool_, path->back().page_id);
  if (!leaf_page) {
    return std::unexpected(std::move(leaf_page).error());
  }
  return LeafNode::Load(leaf_page->Data()).Get(key);
}

auto BPlusTree::Remove(std::string_view key) -> Status {
  /*
    Delete path:

      1. Descend to the owning leaf.
      2. Remove the record and rewrite the leaf.
      3. If the leaf is still at least half full, stop.
      4. Otherwise repair bottom-up.

    A repair can end three ways:

      rebalance: child count unchanged, parent separator updated, stop
      skipped:   child remains underfull but routing is still correct, stop
      merge:     parent loses one separator, so the parent may underflow too

    When the cascade reaches the root, CollapseRoot removes useless single
    child internal roots while keeping root_page_id_ stable.
  */
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);
  if (!path) {
    return path.error();
  }
  {
    auto leaf_page = PageRef::Fetch(buffer_pool_, path->back().page_id);
    if (!leaf_page) {
      return std::move(leaf_page).error();
    }
    auto node = LeafNode::Load(leaf_page->Data());
    if (!node.Erase(key)) {
      return {};
    }
    node.Store(leaf_page->Data());
    leaf_page->MarkDirty();

    const bool is_root_leaf = path->size() == 1;
    if (is_root_leaf || !node.Underfull()) {
      return {};
    }
  }

  // Repair underfull nodes bottom-up. A rebalance (or a skipped repair) ends
  // the walk; a merge shrinks the parent, which may need repair itself.
  bool leaf_level = true;
  for (std::size_t level = path->size() - 1; level > 0; --level) {
    const auto merged = leaf_level ? RepairLeafChild(buffer_pool_, (*path)[level - 1].page_id, (*path)[level])
                                   : RepairInternalChild(buffer_pool_, (*path)[level - 1].page_id, (*path)[level]);
    if (!merged) {
      return merged.error();
    }
    if (!*merged) {
      return {};
    }
    leaf_level = false;

    if (level - 1 == 0) {
      break;  // the parent is the root; handled by CollapseRoot below
    }
    auto parent_page = PageRef::Fetch(buffer_pool_, (*path)[level - 1].page_id);
    if (!parent_page) {
      return std::move(parent_page).error();
    }
    if (!InternalNode::Load(parent_page->Data()).Underfull()) {
      return {};
    }
  }
  return CollapseRoot(buffer_pool_, root_page_id_);
}

auto BPlusTree::Scan(std::string_view start,
                     std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>> {
  /*
    Range scan is a seek plus leaf-chain walk:

      DescendToLeaf(start)
              |
              v
        lower_bound(start) in first leaf
              |
              v
        emit records until key >= end, following next_leaf links

    The end bound is exclusive. Internal pages are not touched after the
    initial seek; the linked leaves are the ordered scan structure.
  */
  auto rows = std::vector<std::pair<std::string, std::string>>{};
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, start);
  if (!path) {
    return std::unexpected(path.error());
  }

  auto page_id = path->back().page_id;
  while (page_id != HEADER_PAGE_ID) {
    auto leaf_page = PageRef::Fetch(buffer_pool_, page_id);
    if (!leaf_page) {
      return std::unexpected(std::move(leaf_page).error());
    }
    const auto node = LeafNode::Load(leaf_page->Data());
    const auto &records = node.Records();
    auto it =
        std::lower_bound(records.begin(), records.end(), start,
                         [](const LeafNode::Record &record, std::string_view target) { return record.key < target; });
    for (; it != records.end(); ++it) {
      if (it->key >= end) {
        return rows;
      }
      rows.emplace_back(it->key, it->value);
    }
    page_id = node.NextLeaf();
  }
  return rows;
}

}  // namespace tinydb
