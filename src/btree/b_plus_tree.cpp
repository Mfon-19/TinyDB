#include "btree/b_plus_tree.h"
#include <tinydb/check.h>

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

#include "internal_page_builder.h"
#include "leaf_page_builder.h"
#include "navigation.h"
#include "page_format.h"
#include "page_source.h"
#include "page_view.h"
#include "value_storage.h"
#include "txn/contract.h"

/*
** CROSS-PAGE B+ TREE ALGORITHMS
**
** Page views validate and search one encoded page. Page builders own one
** mutable logical page. This file establishes relationships between pages:
** descent paths, split propagation, sibling repair, root replacement, leaf
** chaining, and full-tree verification.
**
** Internal separators are inclusive lower bounds for their right child, so an
** equal key always routes right. Leaves contain all values and form a strictly
** increasing forward chain. Root identity is ordinary logical state: growth
** allocates a new root above the old one, and collapse promotes the sole child.
**
** Put writes the destination leaf, then carries at most one pending separator
** upward. Each ancestor absorbs that separator or splits and replaces it with
** another. Remove first completes logical deletion. Occupancy repair is a
** separate space policy: underfull pages remain correct, redistribution may
** stop propagation, and only a merge removes a parent edge and continues up.
**
** Pages are retired only after every handle to them leaves scope. PageSource,
** not the tree, decides when the retired physical ID may be reused.
*/

namespace tinydb {
namespace {

// child_index records the parent edge used to reach page_id. Put needs page_id
// while propagating splits; delete repair needs both fields.
struct PathStep {
  page_id_t page_id;
  std::size_t child_index;
};

auto DescendToLeaf(PageSource *pages, page_id_t root_page_id, std::string_view key) -> Result<std::vector<PathStep>> {
  // Retain page ids, not page handles: each level is released before the next
  // read. The visited set turns a corrupt child cycle into a finite error.
  auto path = std::vector<PathStep>{{root_page_id, 0}};
  auto visited = std::unordered_set<page_id_t>{root_page_id};
  for (;;) {
    auto page = pages->Read(path.back().page_id);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    const auto type = RawNodeType(page->Data());
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      return path;
    }
    if (type != static_cast<std::uint16_t>(NodeType::Internal)) {
      return std::unexpected(Status::Corruption("tree descent reached a non-tree page"));
    }
    const auto node = InternalPageView::Open(page->Data(), page->Id());
    if (!node) {
      return std::unexpected(node.error());
    }
    const auto child_index = node->FindChildIndex(key);
    const auto child = node->ChildAt(child_index);
    if (!visited.insert(child).second) {
      return std::unexpected(Status::Corruption("tree descent contains a page cycle"));
    }
    path.push_back({child, child_index});
  }
}

// Builders never decode raw bytes themselves. Opening the view first keeps
// validation and interpretation in one implementation.
auto LeafBuilder(PageHandle &page) -> Result<LeafPageBuilder> {
  const auto view = LeafPageView::Open(page.Data(), page.Id());
  if (!view) {
    return std::unexpected(view.error());
  }
  return LeafPageBuilder::From(*view);
}

auto InternalBuilder(PageHandle &page) -> Result<InternalPageBuilder> {
  const auto view = InternalPageView::Open(page.Data(), page.Id());
  if (!view) {
    return std::unexpected(view.error());
  }
  return InternalPageBuilder::From(*view);
}

// A split leaves its left half at the original page and carries this edge to
// the parent. Parent overflow replaces it with another pending edge.
struct PendingSeparator {
  std::string key;
  page_id_t right_child;
};

// Allocate the right leaf before changing the left. Split updates both leaf
// contents and the forward link; the returned separator is the right minimum.
auto SplitAndWrite(PageSource *pages, PageHandle &page, LeafPageBuilder &node,
                   bool tail_heavy) -> Result<PendingSeparator> {
  auto right_page = pages->Allocate();
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split(right_page->Id(), tail_heavy);
  split.right.Store(right_page->MutableData(), right_page->Id());
  right_page->MarkDirty();

  node.Store(page.MutableData(), page.Id());
  page.MarkDirty();
  return PendingSeparator{std::move(split.separator), right_page->Id()};
}

// Internal splits promote one separator out of both halves. Its right child
// becomes the new page's first child, preserving N separators/N+1 children.
auto SplitAndWrite(PageSource *pages, PageHandle &page, InternalPageBuilder &node) -> Result<PendingSeparator> {
  auto right_page = pages->Allocate();
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split();
  split.right.Store(right_page->MutableData(), right_page->Id());
  right_page->MarkDirty();

  node.Store(page.MutableData(), page.Id());
  page.MarkDirty();
  return PendingSeparator{std::move(split.separator), right_page->Id()};
}

auto RetirePage(PageSource *pages, page_id_t page_id) -> Status {
  // Tree code declares reachability; the page source decides when reuse is safe.
  return pages->Free(page_id);
}

auto RepairLeafOccupancy(PageSource *pages, page_id_t parent_id, const PathStep &child) -> Result<bool> {
  // Pair with the left sibling when possible; otherwise use the right. The
  // separator between that pair is the only parent record that can change.
  auto parent_page = pages->Edit(parent_id);
  if (!parent_page) {
    return std::unexpected(std::move(parent_page).error());
  }
  auto parent_result = InternalBuilder(*parent_page);
  if (!parent_result) {
    return std::unexpected(parent_result.error());
  }
  auto parent = std::move(*parent_result);
  if (parent.SeparatorCount() == 0) {
    // A one-child parent has no sibling pair. Root collapse handles it later.
    return false;
  }

  // Separator i lies between child i and child i+1.
  const std::size_t sep_index = child.child_index > 0 ? child.child_index - 1 : 0;
  const page_id_t left_id = parent.ChildAt(sep_index);
  const page_id_t right_id = parent.ChildAt(sep_index + 1);

  {
    auto left_page = pages->Edit(left_id);
    if (!left_page) {
      return std::unexpected(std::move(left_page).error());
    }
    auto right_page = pages->Edit(right_id);
    if (!right_page) {
      return std::unexpected(std::move(right_page).error());
    }
    auto left_builder = LeafBuilder(*left_page);
    if (!left_builder) {
      return std::unexpected(left_builder.error());
    }
    auto right_builder = LeafBuilder(*right_page);
    if (!right_builder) {
      return std::unexpected(right_builder.error());
    }
    auto combined = std::move(*left_builder);
    combined.Absorb(std::move(*right_builder));

    if (!combined.Fits()) {
      // The pages cannot merge, so repartition their combined bytes and update
      // the right page's inclusive lower bound in the parent.
      auto split = combined.Split(right_id, /*tail_heavy=*/false);
      parent.SetSeparatorKey(sep_index, std::move(split.separator));
      if (!parent.Fits()) {
        // A larger replacement key can overflow the parent. No page has been
        // stored yet, so leaving the child sparse is the safe result.
        return false;
      }
      combined.Store(left_page->MutableData(), left_page->Id());
      left_page->MarkDirty();
      split.right.Store(right_page->MutableData(), right_page->Id());
      right_page->MarkDirty();
      parent.Store(parent_page->MutableData(), parent_page->Id());
      parent_page->MarkDirty();
      return false;
    }

    // A merge adopts the right leaf's successor before removing its parent edge.
    combined.Store(left_page->MutableData(), left_page->Id());
    left_page->MarkDirty();
    parent.EraseSeparator(sep_index);
    parent.Store(parent_page->MutableData(), parent_page->Id());
    parent_page->MarkDirty();
  }

  // Both sibling handles ended with the block above; retirement cannot race a lease.
  if (auto status = RetirePage(pages, right_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return true;
}

// Internal repair uses the same pair selection and return contract as leaf
// repair. Combining internal pages additionally pulls their parent separator
// down between the two child arrays.
auto RepairInternalOccupancy(PageSource *pages, page_id_t parent_id, const PathStep &child) -> Result<bool> {
  auto parent_page = pages->Edit(parent_id);
  if (!parent_page) {
    return std::unexpected(std::move(parent_page).error());
  }
  auto parent_result = InternalBuilder(*parent_page);
  if (!parent_result) {
    return std::unexpected(parent_result.error());
  }
  auto parent = std::move(*parent_result);
  if (parent.SeparatorCount() == 0) {
    // A one-child parent has no sibling pair. Root collapse handles it later.
    return false;
  }

  // Separator i lies between child i and child i+1.
  const std::size_t sep_index = child.child_index > 0 ? child.child_index - 1 : 0;
  const page_id_t left_id = parent.ChildAt(sep_index);
  const page_id_t right_id = parent.ChildAt(sep_index + 1);

  {
    auto left_page = pages->Edit(left_id);
    if (!left_page) {
      return std::unexpected(std::move(left_page).error());
    }
    auto right_page = pages->Edit(right_id);
    if (!right_page) {
      return std::unexpected(std::move(right_page).error());
    }
    auto left_builder = InternalBuilder(*left_page);
    if (!left_builder) {
      return std::unexpected(left_builder.error());
    }
    auto right_builder = InternalBuilder(*right_page);
    if (!right_builder) {
      return std::unexpected(right_builder.error());
    }
    auto combined = std::move(*left_builder);
    combined.Absorb(parent.SeparatorKeyAt(sep_index), std::move(*right_builder));

    if (!combined.Fits()) {
      auto split = combined.Split();
      parent.SetSeparatorKey(sep_index, std::move(split.separator));
      if (!parent.Fits()) {
        return false;
      }
      combined.Store(left_page->MutableData(), left_page->Id());
      left_page->MarkDirty();
      split.right.Store(right_page->MutableData(), right_page->Id());
      right_page->MarkDirty();
      parent.Store(parent_page->MutableData(), parent_page->Id());
      parent_page->MarkDirty();
      return false;
    }

    combined.Store(left_page->MutableData(), left_page->Id());
    left_page->MarkDirty();
    parent.EraseSeparator(sep_index);
    parent.Store(parent_page->MutableData(), parent_page->Id());
    parent_page->MarkDirty();
  }

  // Both sibling handles ended with the block above; retirement cannot race a lease.
  if (auto status = RetirePage(pages, right_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return true;
}

// Remove redundant one-child root levels. This changes only root identity and
// reachability; child contents do not need to be copied or rebuilt.
auto CollapseRoot(PageSource *pages, page_id_t &root_page_id) -> Status {
  for (;;) {
    page_id_t child_id = HEADER_PAGE_ID;
    const auto old_root_id = root_page_id;
    {
      auto root_page = pages->Read(root_page_id);
      if (!root_page) {
        return std::move(root_page).error();
      }
      if (RawNodeType(root_page->Data()) != static_cast<std::uint16_t>(NodeType::Internal)) {
        return {};
      }
      const auto root = InternalPageView::Open(root_page->Data(), root_page->Id());
      if (!root) {
        return root.error();
      }
      if (root->SeparatorCount() > 0) {
        return {};
      }
      child_id = root->ChildAt(0);
      auto child_page = pages->Read(child_id);
      if (!child_page) {
        return std::move(child_page).error();
      }
      const auto child_type = RawNodeType(child_page->Data());
      if (child_type != static_cast<std::uint16_t>(NodeType::Leaf) &&
          child_type != static_cast<std::uint16_t>(NodeType::Internal)) {
        return Status::Corruption("root child is not a tree node");
      }
      // The next iteration performs full validation if the promoted child is
      // internal; normal leaf access validates it before reading records.
      root_page_id = child_id;
    }
    // Release both reads before retiring the former root.
    if (auto status = RetirePage(pages, old_root_id); !status.Ok()) {
      return status;
    }
  }
}

struct EraseLeafResult {
  bool removed{false};
  bool underfull{false};
  std::optional<OverflowValueDescriptor> retired_value;
};

// Delete correctness ends once the key is absent and the leaf is repacked.
// Overflow ownership is returned to the caller for transactional retirement;
// occupancy repair remains a separate policy phase.
auto EraseLeaf(PageSource *pages, page_id_t leaf_id, std::string_view key, bool is_root) -> Result<EraseLeafResult> {
  auto page = pages->Edit(leaf_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }
  auto builder = LeafBuilder(*page);
  if (!builder) {
    return std::unexpected(builder.error());
  }
  const auto retired_value = builder->OverflowFor(key);
  if (!builder->Erase(key)) {
    return EraseLeafResult{};
  }
  builder->Store(page->MutableData(), page->Id());
  page->MarkDirty();
  return EraseLeafResult{
      .removed = true,
      .underfull = !is_root && builder->Underfull(),
      .retired_value = retired_value,
  };
}

// A merge removes one parent separator and can make that parent sparse, so only
// merges propagate upward. Redistribution and skipped repair end the pass.
auto RepairDeleteOccupancy(PageSource *pages, const std::vector<PathStep> &path, page_id_t &root_page_id) -> Status {
  bool leaf_level = true;
  for (std::size_t level = path.size() - 1; level > 0; --level) {
    const auto merged = leaf_level ? RepairLeafOccupancy(pages, path[level - 1].page_id, path[level])
                                   : RepairInternalOccupancy(pages, path[level - 1].page_id, path[level]);
    if (!merged) {
      return merged.error();
    }
    if (!*merged) {
      return {};
    }
    leaf_level = false;

    if (level - 1 == 0) {
      break;
    }
    auto parent_page = pages->Read(path[level - 1].page_id);
    if (!parent_page) {
      return std::move(parent_page).error();
    }
    auto parent = InternalBuilder(*parent_page);
    if (!parent) {
      return parent.error();
    }
    if (!parent->Underfull()) {
      return {};
    }
  }
  return CollapseRoot(pages, root_page_id);
}

}  // namespace

// Attach to a validated tree root. An all-zero allocated page is the sole
// bootstrap representation and is immediately initialized as an empty leaf.
auto BPlusTree::Open(PageSource *pages, page_id_t root_page_id) -> Result<BPlusTree> {
  TINYDB_CHECK(pages != nullptr, "page source is null");
  TINYDB_CHECK(root_page_id != HEADER_PAGE_ID, "root page id is the reserved header page");

  // Zero cannot be a NodeType, so it unambiguously identifies a fresh page.
  auto root_page = pages->Edit(root_page_id);
  if (!root_page) {
    return std::unexpected(std::move(root_page).error());
  }
  const auto raw_type = RawNodeType(root_page->Data());
  if (raw_type == 0) {
    LeafPageBuilder{}.Store(root_page->MutableData(), root_page->Id());
    root_page->MarkDirty();
    return BPlusTree(pages, root_page_id);
  }
  const bool is_node = raw_type == static_cast<std::uint16_t>(NodeType::Leaf) ||
                       raw_type == static_cast<std::uint16_t>(NodeType::Internal);
  if (!is_node) {
    return std::unexpected(Status::Corruption("root page is not a b+ tree node"));
  }
  // Existing roots cross the full page-validation boundary before use.
  if (auto status = ValidateTreePage(root_page->Data(), root_page->Id()); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return BPlusTree(pages, root_page_id);
}

auto BPlusTree::Put(std::string_view key, std::string_view value) -> Status {
  // Upsert the destination leaf, then carry at most one separator upward. Each
  // ancestor either absorbs that edge or splits and replaces it with another.
  TINYDB_CHECK(txn::ValidateKeySize(key.size()) == StatusCode::Ok && value.size() <= MAX_VALUE_BYTES,
               "Put sizes must be validated at the public boundary");
  const auto path = DescendToLeaf(pages_, root_page_id_, key);
  if (!path) {
    return path.error();
  }

  std::optional<PendingSeparator> pending;
  std::optional<OverflowValueDescriptor> retired_value;
  {
    auto leaf_page = pages_->Edit(path->back().page_id);
    if (!leaf_page) {
      return std::move(leaf_page).error();
    }
    auto node_result = LeafBuilder(*leaf_page);
    if (!node_result) {
      return node_result.error();
    }
    auto node = std::move(*node_result);
    retired_value = node.OverflowFor(key);
    auto prepared_value = PrepareValue(pages_, key, value);
    if (!prepared_value) {
      return prepared_value.error();
    }
    const bool at_tail = node.Upsert(key, std::move(*prepared_value));

    if (node.Fits()) {
      node.Store(leaf_page->MutableData(), leaf_page->Id());
      leaf_page->MarkDirty();
    } else {
      const bool tail_heavy = at_tail && node.NextLeaf() == HEADER_PAGE_ID;
      auto split = SplitAndWrite(pages_, *leaf_page, node, tail_heavy);
      if (!split) {
        return std::move(split).error();
      }
      pending = std::move(*split);
    }
  }

  // The leaf descriptor changed before the old chain becomes allocator state.
  // A later parent-split failure still aborts the complete private transaction.
  if (retired_value.has_value()) {
    if (auto status = RetireOverflowValue(pages_, *retired_value); !status.Ok()) {
      return status;
    }
  }
  if (!pending.has_value()) {
    return {};
  }

  // level names the child that just split; decrementing selects its parent.
  std::size_t level = path->size() - 1;
  while (pending.has_value() && level > 0) {
    --level;
    auto page = pages_->Edit((*path)[level].page_id);
    if (!page) {
      return std::move(page).error();
    }
    auto node_result = InternalBuilder(*page);
    if (!node_result) {
      return node_result.error();
    }
    auto node = std::move(*node_result);
    node.InsertSeparator(std::move(pending->key), pending->right_child);

    if (node.Fits()) {
      node.Store(page->MutableData(), page->Id());
      page->MarkDirty();
      return {};
    }
    auto split = SplitAndWrite(pages_, *page, node);
    if (!split) {
      return std::move(split).error();
    }
    pending = std::move(*split);
  }

  if (pending.has_value()) {
    // No parent absorbed the final edge. The old root is already its left half.
    auto new_root = pages_->Allocate();
    if (!new_root) {
      return std::move(new_root).error();
    }
    InternalPageBuilder(root_page_id_, std::move(pending->key), pending->right_child)
        .Store(new_root->MutableData(), new_root->Id());
    new_root->MarkDirty();
    root_page_id_ = new_root->Id();
  }
  return {};
}

// Point lookup retains no ancestor path and allocates only when copying a
// present value into the API result.
auto BPlusTree::Get(std::string_view key) -> Result<std::optional<std::string>> {
  const auto leaf_id = FindLeaf(pages_, root_page_id_, key);
  if (!leaf_id) {
    return std::unexpected(leaf_id.error());
  }
  auto leaf_page = pages_->Read(*leaf_id);
  if (!leaf_page) {
    return std::unexpected(std::move(leaf_page).error());
  }
  const auto leaf = LeafPageView::Open(leaf_page->Data(), leaf_page->Id());
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  const auto value = leaf->Get(key);
  if (!value) {
    return std::nullopt;
  }
  auto copied = CopyValue(pages_, *value);
  if (!copied) {
    return std::unexpected(copied.error());
  }
  return std::optional<std::string>{std::move(*copied)};
}

auto BPlusTree::Remove(std::string_view key) -> Status {
  // Logical deletion is complete before space repair starts. This separation
  // keeps underfull pages valid and makes repair policy replaceable.
  const auto path = DescendToLeaf(pages_, root_page_id_, key);
  if (!path) {
    return path.error();
  }
  const auto needs_repair = EraseLeaf(pages_, path->back().page_id, key, path->size() == 1);
  if (!needs_repair) {
    return needs_repair.error();
  }
  if (!needs_repair->removed) {
    return {};
  }
  if (needs_repair->retired_value.has_value()) {
    if (auto status = RetireOverflowValue(pages_, *needs_repair->retired_value); !status.Ok()) {
      return status;
    }
  }
  return needs_repair->underfull ? RepairDeleteOccupancy(pages_, *path, root_page_id_) : Status{};
}

auto BPlusTree::CheckIntegrity(page_id_t next_page_id, const std::unordered_set<page_id_t> &free_pages) -> Status {
  return CheckIntegrity(pages_, root_page_id_, next_page_id, free_pages, {});
}

auto BPlusTree::CheckIntegrity(PageReader *pages, page_id_t root_page_id, page_id_t next_page_id,
                               const std::unordered_set<page_id_t> &free_pages,
                               const std::unordered_set<page_id_t> &allocator_pages) -> Status {
  if (root_page_id == HEADER_PAGE_ID || root_page_id >= next_page_id) {
    return Status::Corruption("root page is outside the allocation frontier");
  }
  if (free_pages.contains(root_page_id) || allocator_pages.contains(root_page_id)) {
    return Status::Corruption("root page is on the free list");
  }

  /*
  ** The recursive walk proves parent routing ranges, page ownership, and
  ** single reachability. It records leaves in in-order traversal so a second
  ** pass can compare the physical successor chain with logical tree order.
  */
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

    auto page = pages->Read(page_id);
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
        const auto value = leaf->ValueAt(index);
        if (value.IsOverflow()) {
          if (auto status = ValidateOverflowValue(pages, value.OverflowDescriptor(), next_page_id, free_pages,
                                                  allocator_pages, &visited);
              !status.Ok()) {
            return std::unexpected(std::move(status));
          }
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
    // Child ranges are half-open. The first inherits the caller's lower bound;
    // the last inherits its upper bound; middle ranges use adjacent separators.
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

  auto root = visit(root_page_id, Bound{}, Bound{});
  if (!root) {
    return std::move(root).error();
  }

  for (std::size_t i = 0; i < leaf_pages.size(); ++i) {
    const auto expected_next = i + 1 < leaf_pages.size() ? leaf_pages[i + 1].first : HEADER_PAGE_ID;
    if (leaf_pages[i].second != expected_next) {
      return Status::Corruption("leaf chain does not match tree order");
    }
  }

  // Finally account for every ID below the high-water frontier exactly once:
  // reachable tree, reusable extent, or allocator metadata.
  for (const auto page_id : free_pages) {
    if (page_id == HEADER_PAGE_ID || page_id >= next_page_id) {
      return Status::Corruption("free-list page is outside the allocation frontier");
    }
  }
  for (const auto page_id : allocator_pages) {
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= next_page_id || free_pages.contains(page_id) ||
        visited.contains(page_id)) {
      return Status::Corruption("allocator page has invalid ownership");
    }
  }
  const auto allocated_pages = next_page_id - FIRST_DATA_PAGE_ID;
  if (visited.size() + free_pages.size() + allocator_pages.size() != allocated_pages) {
    return Status::Corruption("allocated page is neither reachable nor free");
  }
  return {};
}

}  // namespace tinydb
