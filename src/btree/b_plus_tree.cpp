#include "btree/b_plus_tree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "internal_page_builder.h"
#include "leaf_page_builder.h"
#include "navigation.h"
#include "page_format.h"
#include "page_source.h"
#include "page_view.h"
#include "value_storage.h"

/*
** CROSS-PAGE B+ TREE ALGORITHMS
**
** Page views validate and search one encoded page. Page builders own one
** mutable logical page. This file establishes relationships between pages:
** descent paths, split propagation, root replacement, and leaf chaining. The
** complete cross-page ownership audit remains in verify/verifier.cpp rather
** than being duplicated in mutation code.
**
** Internal separators are inclusive lower bounds for their right child, so an
** equal key always routes right. Leaves contain all values and form a strictly
** increasing forward chain. Root identity is ordinary logical state: growth
** allocates a new root above the old one.
**
** PageSource::Edit supplies page-level copy-on-write. An edit keeps the logical
** page ID but rebuilds a transaction-private image; committed readers retain
** the old immutable frame for that ID until publication. Only a split creates
** another logical page, and only a root split changes the root ID. This keeps
** tree algorithms independent of cache version ownership.
**
** Put writes the destination leaf, then carries at most one pending separator
** upward. Each ancestor absorbs that separator or splits and replaces it with
** another. Remove does not merge or rebalance tree pages; it repacks the
** destination leaf and retires any overflow storage removed with the value.
** Sparse pages remain correct because parent separators are inclusive lower
** bounds, not exact copies of the current child minimum.
** Omitting merge and root contraction limits tree-structure copy-on-write to
** the destination leaf; allocator metadata may still record retired overflow
** pages. The tradeoff is retained sparse pages and a height that does not
** decrease.
**
** Pages are retired only after every handle to them leaves scope. PageSource,
** not the tree, decides when the retired physical ID may be reused.
*/

namespace tinydb {
namespace {

struct DescentPath final {
  static constexpr auto MAX_DEPTH = std::size_t{64};

  auto Back() const -> page_id_t { return pages[size - 1]; }

  std::array<page_id_t, MAX_DEPTH> pages;
  std::size_t size{0};
};

/*
** Fill path with page IDs from root through the destination leaf. Handles are
** released at each level; retaining only IDs avoids pinning the tree while a
** write is prepared. The fixed capacity prevents malformed child links from
** causing unbounded traversal and exceeds the depth produced by the builders.
*/
auto DescendToLeaf(PageSource *pages, page_id_t root_page_id, std::string_view key, DescentPath &path) -> Status {
  path.pages[0] = root_page_id;
  path.size = 1;
  for (;;) {
    auto page = pages->Read(path.Back());
    if (!page) {
      return std::move(page).error();
    }
    const auto type = RawNodeType(*page);
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      return {};
    }
    const auto node = InternalPageView::Open(*page);
    if (!node) {
      return node.error();
    }
    if (path.size == path.pages.size()) {
      return Status::Corruption("tree descent is too deep or cyclic");
    }
    const auto child = node->ChildAt(node->FindChildIndex(key));
    path.pages[path.size++] = child;
  }
}

auto LeafBuilder(PageHandle &page) -> Result<LeafPageBuilder> {
  const auto view = LeafPageView::Open(page);
  if (!view) {
    return std::unexpected(view.error());
  }
  return LeafPageBuilder::From(*view);
}

auto InternalBuilder(PageHandle &page) -> Result<InternalPageBuilder> {
  const auto view = InternalPageView::Open(page);
  if (!view) {
    return std::unexpected(view.error());
  }
  return InternalPageBuilder::From(*view);
}

template <typename Builder>
void StoreTreePage(const Builder &builder, PageHandle &page) {
  builder.Store(page.MutableData(), page.Id());
  page.MarkTreePayloadValidated();
  page.MarkDirty();
}

/* A page split carries one separator and its right child toward the root. */
struct PendingSeparator {
  std::string key;
  page_id_t right_child;
};

auto SplitAndWrite(PageSource *pages, PageHandle &page, LeafPageBuilder &node,
                   bool tail_heavy) -> Result<PendingSeparator> {
  auto right_page = pages->Allocate();
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split(right_page->Id(), tail_heavy);
  StoreTreePage(split.right, *right_page);

  StoreTreePage(node, page);
  return PendingSeparator{std::move(split.separator), right_page->Id()};
}

auto SplitAndWrite(PageSource *pages, PageHandle &page, InternalPageBuilder &node) -> Result<PendingSeparator> {
  auto right_page = pages->Allocate();
  if (!right_page) {
    return std::unexpected(std::move(right_page).error());
  }
  auto split = node.Split();
  StoreTreePage(split.right, *right_page);

  StoreTreePage(node, page);
  return PendingSeparator{std::move(split.separator), right_page->Id()};
}

}  // namespace
auto BPlusTree::Open(PageSource *pages, page_id_t root_page_id) -> Result<BPlusTree> {
  if (root_page_id == HEADER_PAGE_ID) {
    auto root_page = pages->Allocate();
    if (!root_page) {
      return std::unexpected(std::move(root_page).error());
    }
    root_page_id = root_page->Id();
    StoreTreePage(LeafPageBuilder{}, *root_page);
    return BPlusTree(pages, root_page_id);
  }

  auto root_page = pages->Read(root_page_id);
  if (!root_page) {
    return std::unexpected(std::move(root_page).error());
  }
  if (auto status = ValidateTreePage(*root_page); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return BPlusTree(pages, root_page_id);
}

auto BPlusTree::Put(std::string_view key, std::string_view value) -> Status {
  DescentPath path;
  if (auto status = DescendToLeaf(pages_, root_page_id_, key, path); !status.Ok()) {
    return status;
  }

  std::optional<PendingSeparator> pending;
  std::optional<OverflowValueDescriptor> retired_value;
  {
    auto leaf_page = pages_->Edit(path.Back());
    if (!leaf_page) {
      return std::move(leaf_page).error();
    }
    auto node_result = LeafBuilder(*leaf_page);
    if (!node_result) {
      return node_result.error();
    }
    auto node = std::move(*node_result);
    auto prepared_value = PrepareValue(pages_, key, value);
    if (!prepared_value) {
      return prepared_value.error();
    }
    const auto upsert = node.Upsert(key, *prepared_value);
    if (!upsert.changed) {
      return {};
    }
    retired_value = upsert.replaced_overflow;

    if (node.Fits()) {
      StoreTreePage(node, *leaf_page);
    } else {
      const bool tail_heavy = upsert.at_tail && node.NextLeaf() == HEADER_PAGE_ID;
      auto split = SplitAndWrite(pages_, *leaf_page, node, tail_heavy);
      if (!split) {
        return std::move(split).error();
      }
      pending = std::move(*split);
    }
  }

  // The leaf descriptor changes before the old chain becomes allocator state.
  // If later propagation fails, the caller must abort rather than commit the
  // partially updated private tree.
  if (retired_value.has_value()) {
    if (auto status = RetireOverflowValue(pages_, *retired_value); !status.Ok()) {
      return status;
    }
  }
  if (!pending.has_value()) {
    return {};
  }

  std::size_t level = path.size - 1;
  while (level > 0) {
    --level;
    auto page = pages_->Edit(path.pages[level]);
    if (!page) {
      return std::move(page).error();
    }
    auto node_result = InternalBuilder(*page);
    if (!node_result) {
      return node_result.error();
    }
    auto node = std::move(*node_result);
    node.InsertSeparator(pending->key, pending->right_child);

    if (node.Fits()) {
      StoreTreePage(node, *page);
      return {};
    }
    auto split = SplitAndWrite(pages_, *page, node);
    if (!split) {
      return std::move(split).error();
    }
    pending = std::move(*split);
  }

  auto new_root = pages_->Allocate();
  if (!new_root) {
    return std::move(new_root).error();
  }
  const auto root = InternalPageBuilder(root_page_id_, pending->key, pending->right_child);
  StoreTreePage(root, *new_root);
  root_page_id_ = new_root->Id();
  return {};
}

auto BPlusTree::Read(PageReader *pages, page_id_t root_page_id,
                     std::string_view key) -> Result<std::optional<std::string>> {
  auto leaf_page = FindLeaf(pages, root_page_id, key);
  if (!leaf_page) {
    return std::unexpected(std::move(leaf_page).error());
  }
  const auto leaf = LeafPageView::Open(*leaf_page);
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  const auto value = leaf->Get(key);
  if (!value) {
    return std::nullopt;
  }
  auto copied = CopyValue(pages, *value);
  if (!copied) {
    return std::unexpected(copied.error());
  }
  return std::optional<std::string>{std::move(*copied)};
}

auto BPlusTree::Get(std::string_view key) -> Result<std::optional<std::string>> {
  return Read(pages_, root_page_id_, key);
}

auto BPlusTree::Remove(std::string_view key) -> Status {
  auto leaf = FindLeaf(pages_, root_page_id_, key);
  if (!leaf) {
    return leaf.error();
  }
  const auto leaf_id = leaf->Id();
  leaf = PageHandle{};
  std::optional<OverflowValueDescriptor> retired_value;
  {
    auto page = pages_->Edit(leaf_id);
    if (!page) {
      return std::move(page).error();
    }
    auto builder = LeafBuilder(*page);
    if (!builder) {
      return builder.error();
    }
    const auto erased = builder->Erase(key);
    if (!erased.erased) {
      return {};
    }
    retired_value = erased.removed_overflow;
    StoreTreePage(*builder, *page);
  }
  if (retired_value.has_value()) {
    if (auto status = RetireOverflowValue(pages_, *retired_value); !status.Ok()) {
      return status;
    }
  }
  return {};
}

}  // namespace tinydb
