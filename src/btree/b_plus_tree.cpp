#include "btree/b_plus_tree.h"
#include "util/check.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "internal_page_builder.h"
#include "leaf_page_builder.h"
#include "navigation.h"
#include "page_format.h"
#include "page_source.h"
#include "page_view.h"
#include "txn/contract.h"
#include "value_storage.h"

/*
** CROSS-PAGE B+ TREE ALGORITHMS
**
** Page views validate and search one encoded page. Page builders own one
** mutable logical page. This file establishes relationships between pages:
** descent paths, split propagation, root replacement, and leaf chaining.
** Cross-page verification belongs to verify/verifier.cpp so normal tree
** mutation and hostile persistent-byte inspection cannot diverge.
**
** Internal separators are inclusive lower bounds for their right child, so an
** equal key always routes right. Leaves contain all values and form a strictly
** increasing forward chain. Root identity is ordinary logical state: growth
** allocates a new root above the old one.
**
** Put writes the destination leaf, then carries at most one pending separator
** upward. Each ancestor absorbs that separator or splits and replaces it with
** another. Remove repacks only the destination leaf. Sparse pages remain
** correct because parent separators are inclusive lower bounds, not exact
** copies of the current child minimum.
**
** Pages are retired only after every handle to them leaves scope. PageSource,
** not the tree, decides when the retired physical ID may be reused.
*/

namespace tinydb {
namespace {

auto DescendToLeaf(PageSource *pages, page_id_t root_page_id, std::string_view key) -> Result<std::vector<page_id_t>> {
  // Retain page ids, not page handles: each level is released before the next
  // read. Sixty-four levels exceed the representable page population at
  // minimum fanout, so the bound also turns a corrupt child cycle into a
  // finite error without a second allocation-bearing visited set.
  auto path = std::vector<page_id_t>{root_page_id};
  for (;;) {
    if (path.size() > 64U) {
      return std::unexpected(Status::Corruption("tree descent is too deep or cyclic"));
    }
    auto page = pages->Read(path.back());
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
    const auto child = node->ChildAt(node->FindChildIndex(key));
    path.push_back(child);
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
    auto leaf_page = pages_->Edit(path->back());
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
    auto page = pages_->Edit((*path)[level]);
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
  const auto leaf_id = FindLeaf(pages_, root_page_id_, key);
  if (!leaf_id) {
    return leaf_id.error();
  }
  std::optional<OverflowValueDescriptor> retired_value;
  {
    auto page = pages_->Edit(*leaf_id);
    if (!page) {
      return std::move(page).error();
    }
    auto builder = LeafBuilder(*page);
    if (!builder) {
      return builder.error();
    }
    retired_value = builder->OverflowFor(key);
    if (!builder->Erase(key)) {
      return {};
    }
    builder->Store(page->MutableData(), page->Id());
    page->MarkDirty();
  }
  if (retired_value.has_value()) {
    if (auto status = RetireOverflowValue(pages_, *retired_value); !status.Ok()) {
      return status;
    }
  }
  return {};
}

}  // namespace tinydb
