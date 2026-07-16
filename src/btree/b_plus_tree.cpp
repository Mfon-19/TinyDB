#include <tinydb/b_plus_tree.h>
#include <tinydb/check.h>
#include <tinydb/page_ref.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "internal_node.h"
#include "leaf_node.h"
#include "page_format.h"
#include "page_view.h"
#include "txn/contract.h"

/**
  B+ tree algorithms: descend, split, repair, collapse.

  Division of labor inside src/btree/:

      page_format.*        encoded offsets and structural validation
      page_view.*          borrowed, allocation-free read access
      leaf_node.cpp        owning leaf builder for mutations
      internal_node.cpp    owning internal builder for mutations
      this file            everything that spans more than one node

  Views and builders own single-page concerns: validation, searching,
  packing, and choosing split points. This file owns the relationships
  between pages — which child a key descends into, how a split's separator
  climbs the tree, and how an underfull node borrows from or merges with a
  sibling. Read paths use views directly over pinned bytes. Mutation paths
  still decode into owning builders until the page-source boundary gives
  each write transaction private page ownership.

  The logical shape, and the separator rule that search depends on:

             internal root
          +-----------------+
          | K10 | K20 | K40 |
          +-----------------+
          /       |     |    \
      < K10   >= K10  >= K20  >= K40
              < K20   < K40

  An internal node is a routing table: records of "separator key -> right
  child", plus one extra child (the first child) for keys below every
  separator. Search descends with upper_bound, which makes the rule "equal
  goes right": a key equal to a separator lives under that separator's
  right child. All values live in the leaves — internal nodes never store
  data — and the leaves are chained left to right in key order, so a range
  scan walks the chain instead of re-descending:

          leaf A        leaf B        leaf C
        [a b c]  ---> [k m n]  ---> [x y z]

  A fresh tree is one page serving as both root and leaf. All growth is
  split-driven: a full leaf splits in two and pushes one separator into
  its parent, a full parent pushes one further up, and the tree gains a
  level exactly when that carry reaches the root. Shrinking mirrors it:
  merges remove separators, and the root collapses away once it is down to
  a single child.

  Every current mutation follows one shape:

      load page bytes into a node
        -> edit the sorted records in memory
        -> prove the result fits, splitting or repairing first if not
        -> Store rewrites the page fully packed

  Nodes are rewritten whole on every change. A 4 KiB page is cheap to
  re-encode, and it buys a lot of simplicity: no fragmentation or
  tombstone bookkeeping, and one canonical encoded result per mutation.

  Invariants this file maintains:

  1. Keys are unique and sorted by unsigned byte order inside every node,
     and the separator rule sends each key to exactly one leaf. Page views
     validate ordering, slots, links, identity, and checksum before access,
     returning persistent damage as Corruption rather than misrouting a
     search.

  2. For now, the root page id never changes for the life of the tree. A root split
     moves both halves out to fresh pages and rewrites the root page in
     place as an internal node above them; a root collapse copies the last
     surviving child over the root page. The page-source cutover removes
     this special case and publishes a newly allocated root ID as state.

  3. The leaf chain stays complete and in key order: a leaf split splices
     the new right leaf into the chain, and a merge adopts the absorbed
     leaf's next pointer before that page is freed.

  4. Nodes stay at least half full after deletes, with two exceptions: the
     root, and one deliberate soft spot — when fixing an underfull node
     would push a separator into a parent that has no room for it, the
     repair is skipped. The node stays underfull, searches stay correct,
     only occupancy suffers.

  5. Pages orphaned by merges and root collapses go back to the buffer
     pool (BufferPool::FreePage), which drops its cached copy and hands
     the page to the disk manager's free list. The pool insists a freed
     page be unpinned first; the extra brace scopes just before each
     FreePage call exist to end the PageRef pins.

  What this file asks of its caller (the storage engine), and promises:

  - Entries must be pre-screened: Put aborts if key + value exceeds
    MAX_ENTRY_BYTES. The cap is what guarantees an
    overflowing node always has a split point where both halves fit (see
    the static_asserts in page_format.h).

  - The root page must be allocated before Open. A zeroed page is
    bootstrapped into an empty leaf; a page holding anything that is not a
    tree node is Corruption.

  - I/O failures come back as statuses, and no dependent page is written
    once one occurs. But a failed mutation may already have rewritten
    other pages in the buffer pool, and a redo-only design has no undo:
    after a failed Put or Remove the tree must not be touched again. The
    engine enforces that by poisoning the handle (see storage_engine.cpp).
*/

namespace tinydb {
namespace {

// One node on a root-to-leaf path: the page, and which child slot of its
// parent points at it (0 for the root, which has no parent).
struct PathStep {
  page_id_t page_id;
  std::size_t child_index;
};

auto DescendToLeaf(BufferPool *pool, page_id_t root_page_id, std::string_view key) -> Result<std::vector<PathStep>> {
  /*
    Walk from the stable root page down to the one leaf that owns key,
    recording every node passed through:

        path[0]     = {root page,  0}
        path[1]     = {child page, index of that child under the root}
        ...
        path.back() = {leaf page,  index under its parent}

    Every tree operation starts here; the recorded positions are what the
    mutations lean on afterward. Put walks the path back up to insert
    split separators into the right ancestors. Remove needs the
    child_index values: when a delete leaves a node underfull, the repair
    reopens the parent and must know which child slot points at the
    underfull node — recording that on the way down means the repair never
    searches a parent for a page id.

    Pinning is transient: each level's page is pinned only long enough to
    pick the next child, so the descent holds one pin at a time and the
    callers re-fetch pages by id when they mutate.

    The depth check is the cycle guard. Parent-child links live on disk,
    and a corrupt page could point back into the path and loop the descent
    forever; no legitimate tree comes close, since even at the minimum
    fanout of two, sixty-four levels would need more pages than a 64-bit
    page id can name.
  */
  auto path = std::vector<PathStep>{{root_page_id, 0}};
  for (;;) {
    TINYDB_CHECK(path.size() <= 64, "tree too deep; page cycle likely");
    auto page = PageRef::Fetch(pool, path.back().page_id);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    const auto type = RawNodeType(page->Data());
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      return path;
    }
    TINYDB_CHECK(type == static_cast<std::uint16_t>(NodeType::Internal), "descended into a non-tree page");
    const auto node = InternalPageView::Open(page->Data(), page->Id());
    if (!node) {
      return std::unexpected(node.error());
    }
    const auto child_index = node->FindChildIndex(key);
    path.push_back({node->ChildAt(child_index), child_index});
  }
}

// A separator waiting to be inserted one level up: produced by a split,
// consumed by the parent — which may overflow in turn and produce another.
struct PendingSeparator {
  std::string key;
  page_id_t right_child;
};

// Writes out both halves of an overflowing leaf and returns what the
// parent must learn about it, if anything.
auto SplitAndWrite(BufferPool *pool, PageRef &page, LeafNode &node, bool is_root,
                   bool tail_heavy) -> Result<std::optional<PendingSeparator>> {
  /*
    An ordinary (non-root) leaf reuses its own page for the left half and
    moves the right half to a fresh page, already spliced into the chain
    by LeafNode::Split:

        before:
          page P: [a b c k m z] -> old_next

        after:
          page P: [a b c] -> page R: [k m z] -> old_next

        returned for the parent:
          (separator "k", right_child R)

    The root leaf cannot do that, because the root page id is the one page
    id the engine persists and this tree never changes it. So a root split
    keeps the page and replaces its contents: both halves move out to
    fresh pages, and the root page is rewritten in place as an internal
    node over them. Nothing is returned — the separator was absorbed into
    the rebuilt root, and the tree just grew a level:

        before:
          root P (leaf): [a b c k m z]

        after:
          root P (internal): first_child = L, ["k" -> R]

          L: [a b c] -> R: [k m z]

    tail_heavy is the ascending-load optimization. When the new key landed
    at the very end of the rightmost leaf, an even split would leave a
    trail of half-full pages behind a bulk sequential insert; splitting as
    "everything old | just the new record" leaves dense leaves instead
    (see LeafNode::Split for the picture).

    Failure note: if allocating a page fails partway through, some pages
    are already rewritten in the pool while the parent still routes every
    key to the old leaf. This function makes no attempt to roll that back
    — it is one of the reasons a failed mutation poisons the whole engine
    (see the file comment).
  */
  auto right_page = PageRef::New(pool);
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split(right_page->Id(), tail_heavy);
  split.right.Store(right_page->Data(), right_page->Id());
  right_page->MarkDirty();

  if (!is_root) {
    node.Store(page.Data(), page.Id());
    page.MarkDirty();
    return std::optional{PendingSeparator{std::move(split.separator), right_page->Id()}};
  }

  auto left_page = PageRef::New(pool);
  if (!left_page) {
    return std::unexpected(std::move(left_page).error());
  }
  node.Store(left_page->Data(), left_page->Id());
  left_page->MarkDirty();

  const InternalNode new_root(left_page->Id(), std::move(split.separator), right_page->Id());
  new_root.Store(page.Data(), page.Id());
  page.MarkDirty();
  return std::optional<PendingSeparator>{};
}

// The internal-node overload: the same page dance as the leaf version,
// minus the leaf chain and the tail-heavy special case.
auto SplitAndWrite(BufferPool *pool, PageRef &page, InternalNode &node,
                   bool is_root) -> Result<std::optional<PendingSeparator>> {
  /*
    The important difference from a leaf split is what happens to the
    middle separator: it leaves the node entirely. A leaf keeps its
    separator key (the parent stores a copy) because leaves must hold
    every value; internal keys are pure routing, so once the parent holds
    the promoted key, a copy left in a child would just be a wasted slot.

        before:
          first=C0, records=[K0->C1, K1->C2, K2->C3, K3->C4]

        split at K2:
          left (this page):  first=C0, records=[K0->C1, K1->C2]
          promoted:          K2
          right (new page):  first=C3, records=[K3->C4]

    K2's old right child C3 becomes the right half's first_child — that
    handoff is what keeps "N separators, N + 1 children" true on both
    sides after K2 climbs out (see InternalNode::Split).

    A root split rewrites the root page in place as a new two-child
    internal node, exactly like the leaf overload: both halves move to
    fresh pages and the root page id stays put.
  */
  auto right_page = PageRef::New(pool);
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split();
  split.right.Store(right_page->Data(), right_page->Id());
  right_page->MarkDirty();

  if (!is_root) {
    node.Store(page.Data(), page.Id());
    page.MarkDirty();
    return std::optional{PendingSeparator{std::move(split.separator), right_page->Id()}};
  }

  auto left_page = PageRef::New(pool);
  if (!left_page) {
    return std::unexpected(std::move(left_page).error());
  }
  node.Store(left_page->Data(), left_page->Id());
  left_page->MarkDirty();

  const InternalNode new_root(left_page->Id(), std::move(split.separator), right_page->Id());
  new_root.Store(page.Data(), page.Id());
  page.MarkDirty();
  return std::optional<PendingSeparator>{};
}

// Fixes an underfull leaf by combining it with an adjacent sibling. Returns
// true iff the two pages merged into one — the only outcome that removes a
// separator from the parent and can therefore leave the parent underfull.
auto RepairLeafChild(BufferPool *pool, page_id_t parent_id, const PathStep &child) -> Result<bool> {
  /*
    The pair to combine is always two adjacent children of the same
    parent: the underfull child with its left sibling if it has one,
    otherwise with its right sibling. Staying under one parent keeps the
    repair local — the only routing affected is sep[i], the separator
    between the pair.

        parent:  ... | sep[i] | ...
                     /        \
               left leaf L   right leaf R     (sep[i] = first key of R)

    Both leaves are loaded and concatenated in memory, and then one of
    three things happens:

    Merge — the combined records fit in one page:

        L      := all records, adopting R's next-leaf pointer
        parent := sep[i] and the R child erased
        R      := freed

      The parent lost a separator, so it may now be underfull itself;
      returning true tells the caller to keep repairing upward.

    Rebalance — the combined records do not fit in one page:

        split combined evenly back into L and R
        parent sep[i] := first key of the new R

      Redistributing is literally a fresh split of the combined node, so
      both leaves come out near half full — which is what "repaired"
      means. The parent's child count is unchanged; the cascade stops.

    Skip — the rebalanced separator does not fit in the parent:

      Nothing is stored. The child stays underfull, which costs occupancy
      but breaks nothing: separators route correctly around a sparse node.
      The fit is checked before any page is written, so a skipped repair
      leaves no partial state behind.
  */
  auto parent_page = PageRef::Fetch(pool, parent_id);
  if (!parent_page) {
    return std::unexpected(std::move(parent_page).error());
  }
  auto parent = InternalNode::Load(parent_page->Data());
  if (parent.SeparatorCount() == 0) {
    // A parent with no separators has exactly one child: there is no
    // sibling to combine with. CollapseRoot handles that shape.
    return false;
  }

  // sep[i] sits between the pair: a child with a left sibling pairs
  // leftward (the separator just before it); the leftmost child has no
  // left sibling and pairs rightward instead.
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
      // Rebalance. Split hands back an even redistribution; the new right
      // half's first key becomes the replacement separator.
      auto split = combined.Split(right_id, /*tail_heavy=*/false);
      parent.SetSeparatorKey(sep_index, std::move(split.separator));
      if (!parent.Fits()) {
        // Skip: the replacement separator is fatter than the one it
        // replaces and the parent has no room for it. Nothing has been
        // stored yet, so bailing out here leaves every page untouched.
        return false;
      }
      combined.Store(left_page->Data(), left_page->Id());
      left_page->MarkDirty();
      split.right.Store(right_page->Data(), right_page->Id());
      right_page->MarkDirty();
      parent.Store(parent_page->Data(), parent_page->Id());
      parent_page->MarkDirty();
      return false;
    }

    // Merge: everything lives in the left page now, and the parent forgets
    // the right child ever existed.
    combined.Store(left_page->Data(), left_page->Id());
    left_page->MarkDirty();
    parent.EraseSeparator(sep_index);
    parent.Store(parent_page->Data(), parent_page->Id());
    parent_page->MarkDirty();
  }

  // Free the merged-away sibling. Deferred to here because the pool
  // insists on an unpinned page, and the scope above just released the pin.
  pool->FreePage(right_id);
  return true;
}

// RepairLeafChild's counterpart for internal nodes: same pairing rule, same
// merge / rebalance / skip outcomes, same return value. The difference is
// what combining means — the parent's separator physically moves down
// between the two halves.
auto RepairInternalChild(BufferPool *pool, page_id_t parent_id, const PathStep &child) -> Result<bool> {
  /*
    Two adjacent leaves can simply concatenate, because leaves hold every
    key. Two adjacent internal nodes cannot: the keys under R's first
    child fall between L's last separator and R's first one, and the only
    key describing that boundary is sep[i] in the parent. So the combine
    pulls the parent separator down to stitch the halves together:

        parent:   ... | sep[i] | ...
                      /        \
            L: first=C0,      R: first=C2,
               [K0->C1]          [K2->C3]

        combined: first=C0, [K0->C1, sep[i]->C2, K2->C3]

    sep[i] takes R's old first child as its right child — exactly the
    subtree holding keys >= sep[i] and below R's first separator.

    From there the outcomes mirror the leaf repair. If combined fits, it
    replaces L, the parent erases sep[i], and R is freed (merge — returns
    true, the parent may now be underfull). If it does not fit, combined
    is split again and the freshly promoted middle key replaces sep[i] in
    the parent (rebalance). And if that promoted key is too fat for the
    parent, the repair is skipped before anything is stored.
  */
  auto parent_page = PageRef::Fetch(pool, parent_id);
  if (!parent_page) {
    return std::unexpected(std::move(parent_page).error());
  }
  auto parent = InternalNode::Load(parent_page->Data());
  if (parent.SeparatorCount() == 0) {
    // A parent with no separators has exactly one child: there is no
    // sibling to combine with. CollapseRoot handles that shape.
    return false;
  }

  // sep[i] sits between the pair; see RepairLeafChild.
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
      combined.Store(left_page->Data(), left_page->Id());
      left_page->MarkDirty();
      split.right.Store(right_page->Data(), right_page->Id());
      right_page->MarkDirty();
      parent.Store(parent_page->Data(), parent_page->Id());
      parent_page->MarkDirty();
      return false;
    }

    combined.Store(left_page->Data(), left_page->Id());
    left_page->MarkDirty();
    parent.EraseSeparator(sep_index);
    parent.Store(parent_page->Data(), parent_page->Id());
    parent_page->MarkDirty();
  }

  // Free the merged-away sibling. Deferred to here because the pool
  // insists on an unpinned page, and the scope above just released the pin.
  pool->FreePage(right_id);
  return true;
}

// Shrinks the tree after deletes: while the root is an internal node with
// no separators left, its lone child is the entire tree, so the child is
// copied over the root page and the child's page is freed.
auto CollapseRoot(BufferPool *pool, page_id_t root_page_id) -> Status {
  /*
    The inverse of a root split, with the same motivation: the engine
    persists one root page id and this tree never changes it, so the tree
    must shrink by pulling content up into the root page rather than by
    declaring some other page the new root.

        before:
          root page P: internal, first_child = C, no separators
          page C:      leaf or internal holding the whole remaining tree

        after:
          root page P: byte-for-byte copy of C
          page C:      freed

    The copy is deliberately raw: C may be a leaf or an internal node, and
    a whole-page memcpy is correct for either without decoding it. This is
    one of the two places this file touches page bytes directly.

    The loop handles cascades — if C was itself an internal node with a
    single child, the freshly copied root collapses again — and stops as
    soon as the root is a leaf or has at least one separator.
  */
  for (;;) {
    page_id_t child_id = HEADER_PAGE_ID;
    {
      auto root_page = PageRef::Fetch(pool, root_page_id);
      if (!root_page) {
        return std::move(root_page).error();
      }
      if (RawNodeType(root_page->Data()) != static_cast<std::uint16_t>(NodeType::Internal)) {
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
      const auto child_type = RawNodeType(child_page->Data());
      if (child_type == static_cast<std::uint16_t>(NodeType::Leaf)) {
        LeafNode::Load(child_page->Data()).Store(root_page->Data(), root_page->Id());
      } else {
        TINYDB_CHECK(child_type == static_cast<std::uint16_t>(NodeType::Internal), "root child is not a tree node");
        InternalNode::Load(child_page->Data()).Store(root_page->Data(), root_page->Id());
      }
      root_page->MarkDirty();
    }
    // The swallowed child's page, unpinned above.
    pool->FreePage(child_id);
  }
}

}  // namespace

// Attaches to the tree rooted at root_page_id. The caller allocates the
// root page; a zeroed page is bootstrapped into an empty leaf here, so the
// engine never needs to know what an empty tree looks like on disk. Fails
// if the root page cannot be read or holds something that is not a tree
// node.
auto BPlusTree::Open(BufferPool *buffer_pool, page_id_t root_page_id) -> Result<BPlusTree> {
  TINYDB_CHECK(buffer_pool != nullptr, "buffer pool is null");
  TINYDB_CHECK(root_page_id != HEADER_PAGE_ID, "root page id is the reserved header page");

  // A freshly allocated page arrives zeroed, and NodeType deliberately has
  // no zero enumerator, so "type bytes are zero" reliably identifies a
  // virgin root. Sniff the raw bytes rather than casting: any value other
  // than zero, Leaf, or Internal means this page was never a tree root.
  auto root_page = PageRef::Fetch(buffer_pool, root_page_id);
  if (!root_page) {
    return std::unexpected(std::move(root_page).error());
  }
  const auto raw_type = RawNodeType(root_page->Data());
  if (raw_type == 0) {
    LeafNode{}.Store(root_page->Data(), root_page->Id());
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
    Insert or update, structured as descend + upsert + carry propagation:

      1. Descend to the leaf that owns key, recording the path.
      2. Upsert into that leaf's records in memory.
      3. If the leaf still fits, rewrite its page — the common case; done.
      4. Otherwise split it. A non-root split hands back a separator that
         must be inserted one level up; a root split rebuilds the root in
         place and hands back nothing.
      5. Insert the pending separator into the parent, which may overflow
         and split in turn. Walk up the recorded path until some ancestor
         absorbs the separator or the root itself splits.

    The pending separator is the only state carried between levels:

        pending = (separator key, right child page id)

    Each iteration of the loop below either consumes it (the parent had
    room) or replaces it with the next level's separator (the parent
    split). The check that the carry never outruns the recorded path holds
    because a root split always ends the cascade: SplitAndWrite absorbs
    the separator into the rebuilt root and returns nothing.
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
      node.Store(leaf_page->Data(), leaf_page->Id());
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

  // Carry the pending separator up the recorded path, splitting whichever
  // ancestors overflow along the way.
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
      node.Store(page->Data(), page->Id());
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

// A point lookup is the descent and nothing else: find the one leaf that
// can hold key, then binary search its records. Missing keys and present
// keys cost the same page reads.
auto BPlusTree::Get(std::string_view key) -> Result<std::optional<std::string>> {
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, key);
  if (!path) {
    return std::unexpected(path.error());
  }
  auto leaf_page = PageRef::Fetch(buffer_pool_, path->back().page_id);
  if (!leaf_page) {
    return std::unexpected(std::move(leaf_page).error());
  }
  const auto leaf = LeafPageView::Open(leaf_page->Data(), leaf_page->Id());
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  const auto value = leaf->Get(key);
  return value ? std::optional<std::string>{*value} : std::nullopt;
}

auto BPlusTree::Remove(std::string_view key) -> Status {
  /*
    Delete, structured as descend + erase + bottom-up repair:

      1. Descend to the leaf that owns key, recording the path.
      2. Erase the key and rewrite the leaf. A missing key is success —
         the key is absent either way — and writes nothing.
      3. If the leaf is still at least half full, or is the root (which
         has no minimum), done.
      4. Otherwise repair upward along the recorded path.

    Each repair combines the underfull node with an adjacent sibling and
    ends one of three ways (see RepairLeafChild for the details):

        rebalance: records redistributed, parent separator swapped; the
                   parent's child count is unchanged, so stop.
        skip:      the repair would not fit in the parent; the node stays
                   underfull but routing is intact, so stop.
        merge:     two children became one and the parent lost a
                   separator — the parent may now be underfull itself,
                   so continue to the next level up.

    Only merges propagate, which is why the loop keys off the repair's
    returned bool and re-checks the parent at each level. If a merge
    cascade empties the root down to a single child, CollapseRoot shrinks
    the tree's height while keeping root_page_id_ stable.
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
    node.Store(leaf_page->Data(), leaf_page->Id());
    leaf_page->MarkDirty();

    const bool is_root_leaf = path->size() == 1;
    if (is_root_leaf || !node.Underfull()) {
      return {};
    }
  }

  // The bottom-up repair walk. The first iteration repairs the leaf, so it
  // pairs leaves; every later iteration repairs an internal node that lost
  // a separator to a merge below it.
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
      // The parent is the root, and the root has no underfull threshold —
      // its only degenerate shape is "single child", which CollapseRoot
      // fixes below.
      break;
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
    A range scan over [start, end) — end exclusive — is one descent
    followed by a walk along the leaf chain:

        DescendToLeaf(start)
                |
                v
        first leaf: lower_bound skips records below start
                |
                v
        emit records in key order, hopping next_leaf links,
        until a key reaches end or the chain runs out

    Internal nodes are only touched by the initial descent. After that the
    linked leaves are the entire iteration structure — this walk is the
    reason the chain exists.

    The chain's end sentinel is HEADER_PAGE_ID: page 0 is superblock A and
    can never be a tree page, so "next leaf is page 0" safely
    doubles as "no next leaf".

    The walk carries its own cycle guard (DescendToLeaf's depth check
    only protects the descent). A healthy chain is strictly ascending —
    each leaf's first key sorts after everything the previous leaf held —
    and a corrupt next_leaf link that loops the chain back on itself
    breaks that ordering at the first revisited leaf, because keys are
    unique. So checking the order at every hop is a cycle check too.
  */
  auto rows = std::vector<std::pair<std::string, std::string>>{};
  const auto less = txn::BytewiseLess{};
  const auto path = DescendToLeaf(buffer_pool_, root_page_id_, start);
  if (!path) {
    return std::unexpected(path.error());
  }

  auto page_id = path->back().page_id;
  auto previous_last_key = std::optional<std::string>{};
  while (page_id != HEADER_PAGE_ID) {
    auto leaf_page = PageRef::Fetch(buffer_pool_, page_id);
    if (!leaf_page) {
      return std::unexpected(std::move(leaf_page).error());
    }
    const auto leaf = LeafPageView::Open(leaf_page->Data(), leaf_page->Id());
    if (!leaf) {
      return std::unexpected(leaf.error());
    }
    if (leaf->Count() != 0) {
      const bool ascending = !previous_last_key.has_value() || less(*previous_last_key, leaf->KeyAt(0));
      TINYDB_CHECK(ascending, "leaf chain out of key order; page cycle likely");
      previous_last_key = leaf->KeyAt(leaf->Count() - 1);
    }
    for (auto index = leaf->LowerBound(start); index < leaf->Count(); ++index) {
      const auto key = leaf->KeyAt(index);
      if (!less(key, end)) {
        return rows;
      }
      rows.emplace_back(key, leaf->ValueAt(index));
    }
    page_id = leaf->NextLeaf();
  }
  return rows;
}

auto BPlusTree::CheckIntegrity(page_id_t next_page_id, const std::unordered_set<page_id_t> &free_pages) -> Status {
  if (root_page_id_ == HEADER_PAGE_ID || root_page_id_ >= next_page_id) {
    return Status::Corruption("root page is outside the allocation frontier");
  }
  if (free_pages.contains(root_page_id_)) {
    return Status::Corruption("root page is on the free list");
  }

  struct Summary {
    std::optional<std::string> minimum;
    std::optional<std::string> maximum;
  };

  auto visited = std::unordered_set<page_id_t>{};
  auto leaf_pages = std::vector<std::pair<page_id_t, page_id_t>>{};
  using Bound = std::optional<std::string>;
  const auto less = txn::BytewiseLess{};

  std::function<Result<Summary>(page_id_t, const Bound &, const Bound &)> visit;
  visit = [&](page_id_t page_id, const Bound &lower, const Bound &upper) -> Result<Summary> {
    if (page_id == HEADER_PAGE_ID || page_id >= next_page_id) {
      return std::unexpected(Status::Corruption("tree references a page outside the allocation frontier"));
    }
    if (free_pages.contains(page_id)) {
      return std::unexpected(Status::Corruption("tree references a page on the free list"));
    }
    if (!visited.insert(page_id).second) {
      return std::unexpected(Status::Corruption("tree contains a duplicate page reference or cycle"));
    }

    auto page = PageRef::Fetch(buffer_pool_, page_id);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    const auto type = RawNodeType(page->Data());
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      const auto leaf = LeafPageView::Open(page->Data(), page->Id());
      if (!leaf) {
        return std::unexpected(leaf.error());
      }
      for (std::size_t index = 0; index < leaf->Count(); ++index) {
        const auto key = leaf->KeyAt(index);
        if ((lower.has_value() && less(key, *lower)) || (upper.has_value() && !less(key, *upper))) {
          return std::unexpected(Status::Corruption("leaf key lies outside its parent routing range"));
        }
      }
      leaf_pages.emplace_back(page_id, leaf->NextLeaf());
      if (leaf->Count() == 0) {
        return Summary{};
      }
      return Summary{.minimum = std::string{leaf->KeyAt(0)}, .maximum = std::string{leaf->KeyAt(leaf->Count() - 1)}};
    }
    if (type != static_cast<std::uint16_t>(NodeType::Internal)) {
      return std::unexpected(Status::Corruption("reachable page is not a B+ tree node"));
    }

    const auto node = InternalPageView::Open(page->Data(), page->Id());
    if (!node) {
      return std::unexpected(node.error());
    }
    auto result = Summary{};
    for (std::size_t child_index = 0; child_index <= node->SeparatorCount(); ++child_index) {
      const auto child_lower = child_index == 0 ? lower : Bound{std::string{node->KeyAt(child_index - 1)}};
      const auto child_upper =
          child_index == node->SeparatorCount() ? upper : Bound{std::string{node->KeyAt(child_index)}};
      if (child_lower.has_value() && child_upper.has_value() && !less(*child_lower, *child_upper)) {
        return std::unexpected(Status::Corruption("internal node has an invalid routing range"));
      }
      auto child = visit(node->ChildAt(child_index), child_lower, child_upper);
      if (!child) {
        return std::unexpected(std::move(child).error());
      }
      if (!result.minimum.has_value() && child->minimum.has_value()) {
        result.minimum = child->minimum;
      }
      if (child->maximum.has_value()) {
        result.maximum = child->maximum;
      }
    }
    return result;
  };

  auto root = visit(root_page_id_, Bound{}, Bound{});
  if (!root) {
    return std::move(root).error();
  }

  for (std::size_t i = 0; i < leaf_pages.size(); ++i) {
    const auto expected_next = i + 1 < leaf_pages.size() ? leaf_pages[i + 1].first : HEADER_PAGE_ID;
    if (leaf_pages[i].second != expected_next) {
      return Status::Corruption("leaf chain does not match tree order");
    }
  }

  for (const auto page_id : free_pages) {
    if (page_id == HEADER_PAGE_ID || page_id >= next_page_id) {
      return Status::Corruption("free-list page is outside the allocation frontier");
    }
  }
  const auto allocated_pages = next_page_id - FIRST_DATA_PAGE_ID;
  if (visited.size() + free_pages.size() != allocated_pages) {
    return Status::Corruption("allocated page is neither reachable nor free");
  }
  return {};
}

}  // namespace tinydb
