#include "btree/tree_cursor.h"

#include "btree/navigation.h"
#include "btree/value_storage.h"
#include "txn/contract.h"

#include "storage/page.h"

#include <expected>
#include <unordered_set>
#include <utility>

namespace tinydb {

auto BTreeCursor::First(PageReader *pages, page_id_t root_page_id,
                        page_id_t logical_page_count) -> Result<BTreeCursor> {
  return Position(pages, root_page_id, logical_page_count, std::nullopt);
}

auto BTreeCursor::Seek(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count,
                       std::string_view key) -> Result<BTreeCursor> {
  return Position(pages, root_page_id, logical_page_count, key);
}

auto BTreeCursor::CopyValue() const -> Result<std::string> { return tinydb::CopyValue(pages_, Value()); }

auto BTreeCursor::First() -> Status { return Reset(std::nullopt); }

auto BTreeCursor::Seek(std::string_view key) -> Status { return Reset(key); }

auto BTreeCursor::Position(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count,
                           std::optional<std::string_view> lower_bound) -> Result<BTreeCursor> {
  auto page =
      lower_bound.has_value() ? FindLeaf(pages, root_page_id, *lower_bound) : FindFirstLeaf(pages, root_page_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }

  auto cursor = BTreeCursor(pages, root_page_id, logical_page_count);
  if (auto status = cursor.OpenInitialLeaf(std::move(*page)); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (lower_bound.has_value()) {
    cursor.index_ = cursor.CurrentLeaf().LowerBound(*lower_bound);
  }
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

auto BTreeCursor::Reset(std::optional<std::string_view> lower_bound) -> Status {
  auto replacement = Position(pages_, root_page_id_, logical_page_count_, lower_bound);
  if (!replacement) {
    return replacement.error();
  }
  *this = std::move(*replacement);
  return {};
}

auto BTreeCursor::OpenInitialLeaf(PageHandle page) -> Status {
  if (auto status = ValidateLeafId(page.Id()); !status.Ok()) {
    return status;
  }
  auto leaf = LeafPageView::Open(page);
  if (!leaf) {
    return leaf.error();
  }
  // Adopt the new lease before retaining the borrowed view. PageHandle move
  // preserves the address returned by the page source.
  page_ = std::move(page);
  leaf_ = *leaf;
  index_ = 0;
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
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count_) {
    return Status::Corruption("leaf page lies outside the logical page range");
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
