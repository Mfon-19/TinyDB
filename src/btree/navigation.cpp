#include "btree/navigation.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "btree/page_view.h"
#include "txn/contract.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

/*
** Descend one immutable page at a time. choose_child receives a validated
** internal view and selects the next edge. Every handle is released before
** the next iteration. The depth bound detects parent cycles without retaining
** an allocation-sized visited set.
*/
template <typename ChooseChild>
auto Descend(PageReader *pages, page_id_t root_page_id, ChooseChild choose_child) -> Result<PageHandle> {
  auto page_id = root_page_id;
  auto depth = std::size_t{0};
  for (;;) {
    // This fixed bound prevents malformed child links from causing unbounded
    // descent. It is far above the depth produced by the page builders.
    if (++depth > 64) {
      return std::unexpected(Status::Corruption("tree descent is too deep or cyclic"));
    }
    auto page = pages->Read(page_id);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    const auto type = RawNodeType(*page);
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      return std::move(*page);
    }
    const auto internal = InternalPageView::Open(*page);
    if (!internal) {
      return std::unexpected(internal.error());
    }
    page_id = choose_child(*internal);
  }
}

}  // namespace
auto FindLeaf(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<PageHandle> {
  return Descend(pages, root_page_id,
                 [key](const InternalPageView &page) { return page.ChildAt(page.FindChildIndex(key)); });
}

auto FindFirstLeaf(PageReader *pages, page_id_t root_page_id) -> Result<PageHandle> {
  return Descend(pages, root_page_id, [](const InternalPageView &page) { return page.ChildAt(0); });
}

auto PlanLeafPageSuccessorsForReadAhead(PageReader *pages, page_id_t root_page_id, page_id_t current_leaf_page_id,
                                        page_id_t logical_page_count, std::string_view current_key,
                                        std::size_t page_count,
                                        std::optional<std::string_view> upper) noexcept -> std::vector<page_id_t> {
  struct Ancestor final {
    page_id_t page_id;
    std::size_t child_index;
  };

  if (pages == nullptr || page_count == 0 || root_page_id < FIRST_DATA_PAGE_ID ||
      current_leaf_page_id < FIRST_DATA_PAGE_ID || logical_page_count <= FIRST_DATA_PAGE_ID) {
    return {};
  }
  const auto valid_page_id = [logical_page_count](page_id_t page_id) {
    return page_id >= FIRST_DATA_PAGE_ID && page_id < logical_page_count;
  };

  try {
    // Retain ancestor IDs. A reader can reuse storage for an earlier borrowed
    // handle, so the planner reads each ancestor again before it uses it.
    // The shared set also rejects cycles and duplicate internal edges.
    auto ancestors = std::vector<Ancestor>{};
    ancestors.reserve(64);
    auto seen = std::unordered_set<page_id_t>{};
    auto page_id = root_page_id;
    for (auto depth = std::size_t{0};; ++depth) {
      if (depth >= 64U || !valid_page_id(page_id) || !seen.insert(page_id).second) {
        return {};
      }
      auto page = pages->Read(page_id);
      if (!page) {
        return {};
      }
      const auto type = RawNodeType(*page);
      if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
        if (page_id != current_leaf_page_id || !LeafPageView::Open(*page)) {
          return {};
        }
        break;
      }
      if (type != static_cast<std::uint16_t>(NodeType::Internal)) {
        return {};
      }
      const auto internal = InternalPageView::Open(*page);
      if (!internal) {
        return {};
      }
      const auto child_index = internal->FindChildIndex(current_key);
      const auto parent_page_id = page_id;
      page_id = internal->ChildAt(child_index);
      ancestors.push_back(Ancestor{.page_id = parent_page_id, .child_index = child_index});
    }

    auto result = std::vector<page_id_t>{};
    result.reserve(page_count);
    const auto collect = [&](auto &&self, page_id_t subtree_root, std::size_t internal_levels) -> bool {
      if (result.size() == page_count) {
        return true;
      }
      if (!valid_page_id(subtree_root) || !seen.insert(subtree_root).second) {
        return false;
      }
      if (internal_levels == 0) {
        result.push_back(subtree_root);
        return true;
      }
      auto page = pages->Read(subtree_root);
      if (!page || RawNodeType(*page) != static_cast<std::uint16_t>(NodeType::Internal)) {
        return false;
      }
      const auto internal = InternalPageView::Open(*page);
      if (!internal) {
        return false;
      }
      const auto child_count = internal->SeparatorCount() + 1U;
      auto children = std::vector<page_id_t>{};
      children.reserve(child_count);
      for (auto child_index = std::size_t{0}; child_index < child_count; ++child_index) {
        if (upper.has_value() && child_index != 0U && !txn::BytewiseLess{}(internal->KeyAt(child_index - 1U), *upper)) {
          break;
        }
        children.push_back(internal->ChildAt(child_index));
      }
      page = PageHandle{};
      for (const auto child : children) {
        if (result.size() == page_count) {
          break;
        }
        if (!self(self, child, internal_levels - 1U)) {
          return false;
        }
      }
      return true;
    };

    for (auto level = ancestors.size(); level-- > 0 && result.size() < page_count;) {
      auto page = pages->Read(ancestors[level].page_id);
      if (!page || RawNodeType(*page) != static_cast<std::uint16_t>(NodeType::Internal)) {
        return {};
      }
      const auto internal = InternalPageView::Open(*page);
      if (!internal) {
        return {};
      }
      const auto child_count = internal->SeparatorCount() + 1U;
      if (ancestors[level].child_index >= child_count) {
        return {};
      }
      auto siblings = std::vector<page_id_t>{};
      siblings.reserve(child_count - ancestors[level].child_index - 1U);
      for (auto child_index = ancestors[level].child_index + 1U; child_index < child_count; ++child_index) {
        if (upper.has_value() && !txn::BytewiseLess{}(internal->KeyAt(child_index - 1U), *upper)) {
          break;
        }
        siblings.push_back(internal->ChildAt(child_index));
      }
      page = PageHandle{};
      const auto internal_levels = ancestors.size() - level - 1U;
      for (const auto sibling : siblings) {
        if (result.size() == page_count) {
          break;
        }
        if (!collect(collect, sibling, internal_levels)) {
          return {};
        }
      }
    }
    return result;
  } catch (const std::bad_alloc &) {
    return {};
  } catch (const std::length_error &) {
    return {};
  }
}

}  // namespace tinydb
