#include "btree/tree_cursor.h"

#include "btree/navigation.h"
#include "txn/contract.h"

#include "storage/page.h"
#include "util/check.h"

#include <expected>
#include <unordered_set>
#include <utility>

namespace tinydb {

auto BTreeCursor::First(PageReader *pages, page_id_t root_page_id,
                        page_id_t high_water_page_id) -> Result<BTreeCursor> {
  auto page = FindFirstLeaf(pages, root_page_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }

  auto cursor = BTreeCursor(pages, high_water_page_id);
  if (auto status = cursor.OpenInitialLeaf(std::move(*page), 0); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (!cursor.Valid()) {
    const auto next_leaf = cursor.CurrentLeaf().NextLeaf();
    if (auto status = cursor.AdvanceToNonEmptyLeaf(next_leaf); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }
  return cursor;
}

auto BTreeCursor::Seek(PageReader *pages, page_id_t root_page_id, page_id_t high_water_page_id,
                       std::string_view key) -> Result<BTreeCursor> {
  // Descent finds the only leaf that may contain key; LowerBound establishes
  // the first cursor position without copying the encoded key.
  auto page = FindLeaf(pages, root_page_id, key);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }

  auto cursor = BTreeCursor(pages, high_water_page_id);
  if (auto status = cursor.OpenInitialLeaf(std::move(*page), 0); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  cursor.index_ = cursor.CurrentLeaf().LowerBound(key);
  if (cursor.Valid()) {
    return cursor;
  }
  // Keep this lease alive while opening the successor so its last key remains
  // available for the global ordering check without an owning copy.
  const auto next_leaf = cursor.CurrentLeaf().NextLeaf();
  if (auto status = cursor.AdvanceToNonEmptyLeaf(next_leaf); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return cursor;
}

auto BTreeCursor::OpenInitialLeaf(PageHandle page, std::size_t index) -> Status {
  if (auto status = ValidateLeafId(page.Id()); !status.Ok()) {
    return status;
  }
  auto leaf = LeafPageView::Open(page);
  if (!leaf) {
    return leaf.error();
  }
  if (index > leaf->Count()) {
    return Status::Corruption("cursor position lies beyond leaf records");
  }

  // Adopt the new lease before retaining the borrowed view. PageHandle move
  // preserves the address returned by the page source.
  page_ = std::move(page);
  leaf_ = *leaf;
  index_ = index;
  return {};
}

auto BTreeCursor::AdvanceToNonEmptyLeaf(page_id_t page_id) -> Status {
  // Keep the previous non-empty leaf pinned across sparse successors. Its last
  // key can then validate the next non-empty boundary without allocating.
  auto empty_pages = std::unordered_set<page_id_t>{};
  if (leaf_.has_value() && leaf_->Count() == 0) {
    empty_pages.insert(page_.Id());
  }
  while (page_id != HEADER_PAGE_ID) {
    if (auto status = ValidateLeafId(page_id); !status.Ok()) {
      return status;
    }
    if (empty_pages.contains(page_id)) {
      return Status::Corruption("leaf chain contains a cycle");
    }
    auto page = pages_->Read(page_id);
    if (!page) {
      return std::move(page).error();
    }
    auto leaf = LeafPageView::Open(*page);
    if (!leaf) {
      return leaf.error();
    }
    if (leaf->Count() == 0) {
      empty_pages.insert(page_id);
      page_id = leaf->NextLeaf();
      continue;
    }
    if (auto status = ValidateBoundary(*leaf); !status.Ok()) {
      return status;
    }
    page_ = std::move(*page);
    leaf_ = *leaf;
    index_ = 0;
    return {};
  }

  page_ = PageHandle{};
  // End-of-chain is represented by an absent view, not a fabricated position.
  leaf_.reset();
  index_ = 0;
  return {};
}

auto BTreeCursor::ValidateLeafId(page_id_t page_id) const -> Status {
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= high_water_page_id_) {
    return Status::Corruption("leaf page lies outside the allocation frontier");
  }
  return {};
}

auto BTreeCursor::ValidateBoundary(const LeafPageView &next) const -> Status {
  if (leaf_.has_value() && leaf_->Count() != 0 &&
      !txn::BytewiseLess{}(leaf_->KeyAt(leaf_->Count() - 1), next.KeyAt(0))) {
    return Status::Corruption("leaf chain is out of key order");
  }
  return {};
}

}  // namespace tinydb
