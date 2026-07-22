#include "btree/tree_cursor.h"

#include "btree/navigation.h"
#include "txn/contract.h"

#include "storage/page.h"
#include "util/check.h"

#include <expected>
#include <utility>

namespace tinydb {

auto BTreeCursor::First(PageReader *pages, page_id_t root_page_id) -> Result<BTreeCursor> {
  const auto leaf_id = FindFirstLeaf(pages, root_page_id);
  if (!leaf_id) {
    return std::unexpected(std::move(leaf_id).error());
  }

  auto cursor = BTreeCursor(pages);
  if (auto status = cursor.AdvanceToNonEmptyLeaf(*leaf_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return cursor;
}

auto BTreeCursor::Seek(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<BTreeCursor> {
  // Descent finds the only leaf that may contain key; LowerBound establishes
  // the first cursor position without copying the encoded key.
  auto leaf_id = FindLeaf(pages, root_page_id, key);
  if (!leaf_id) {
    return std::unexpected(std::move(leaf_id).error());
  }

  auto cursor = BTreeCursor(pages);
  if (auto status = cursor.OpenLeaf(*leaf_id, 0); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  cursor.index_ = cursor.leaf_->LowerBound(key);
  if (cursor.Valid()) {
    return cursor;
  }
  // The seek landed past this leaf (or in an empty leaf). Preserve its boundary
  // before following the chain so OpenLeaf can verify global ordering.
  const auto next_leaf = cursor.leaf_->NextLeaf();
  cursor.RememberLastKey();
  if (auto status = cursor.AdvanceToNonEmptyLeaf(next_leaf); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return cursor;
}

auto BTreeCursor::Key() const -> std::string_view {
  TINYDB_CHECK(Valid(), "reading key from an invalid tree cursor");
  return leaf_->KeyAt(index_);
}

auto BTreeCursor::Value() const -> LeafValueView {
  TINYDB_CHECK(Valid(), "reading value from an invalid tree cursor");
  return leaf_->ValueAt(index_);
}

auto BTreeCursor::Next() -> Status {
  TINYDB_CHECK(Valid(), "advancing an invalid tree cursor");
  ++index_;
  if (index_ < leaf_->Count()) {
    return {};
  }

  // Save the boundary while the current lease is alive; opening the successor
  // replaces page_ and invalidates all slices into this page.
  RememberLastKey();
  return AdvanceToNonEmptyLeaf(leaf_->NextLeaf());
}

auto BTreeCursor::OpenLeaf(page_id_t page_id, std::size_t index) -> Status {
  // Track ids independently of key ordering: empty leaves have no key boundary
  // but can still participate in a cycle.
  if (!visited_.insert(page_id).second) {
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
  if (index > leaf->Count()) {
    return Status::Corruption("cursor position lies beyond leaf records");
  }
  if (leaf->Count() != 0 && previous_last_key_.has_value() &&
      !txn::BytewiseLess{}(*previous_last_key_, leaf->KeyAt(0))) {
    return Status::Corruption("leaf chain is out of key order");
  }

  // Adopt the new lease before retaining the borrowed view. PageHandle move
  // preserves the address returned by the page source.
  page_ = std::move(*page);
  leaf_ = *leaf;
  index_ = index;
  return {};
}

auto BTreeCursor::AdvanceToNonEmptyLeaf(page_id_t page_id) -> Status {
  // Sparse leaves are legal. Skip them while still validating their links and
  // recording their ids for cycle detection.
  while (page_id != HEADER_PAGE_ID) {
    if (auto status = OpenLeaf(page_id, 0); !status.Ok()) {
      return status;
    }
    if (Valid()) {
      return {};
    }
    page_id = leaf_->NextLeaf();
    RememberLastKey();
  }

  page_ = PageHandle{};
  // End-of-chain is represented by an absent view, not a fabricated position.
  leaf_.reset();
  index_ = 0;
  return {};
}

void BTreeCursor::RememberLastKey() {
  if (leaf_.has_value() && leaf_->Count() != 0) {
    previous_last_key_ = std::string(leaf_->KeyAt(leaf_->Count() - 1));
  }
}

}  // namespace tinydb
