#include "txn/read_snapshot.h"

#include "btree/navigation.h"
#include "btree/page_source.h"
#include "btree/page_view.h"

#include <expected>
#include <utility>

namespace tinydb::txn {

/*
** Begin captures admission before exposing the wrapper. PageReader itself is
** not snapshot-versioned; the gate prevents publication from replacing cache
** versions while this snapshot or a cursor derived from it remains alive.
*/
auto ReadSnapshot::Begin(ReaderGate *gate, PageReader *pages) -> ReadSnapshot {
  return ReadSnapshot(gate->BeginRead(), pages);
}

/*
** Find the leaf from the captured root, validate its persistent bytes, and
** copy a matching value before releasing the page lease. A missing key is a
** successful optional result rather than an error.
*/
auto ReadSnapshot::Get(std::string_view key) -> Result<std::optional<std::string>> {
  const auto leaf_id = FindLeaf(pages_, State().root_page_id, key);
  if (!leaf_id) {
    return std::unexpected(leaf_id.error());
  }
  auto page = pages_->Read(*leaf_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }
  const auto leaf = LeafPageView::Open(page->Data(), page->Id());
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  const auto value = leaf->Get(key);
  return value ? std::optional<std::string>{*value} : std::nullopt;
}

auto ReadSnapshot::Seek(std::string_view key) -> Result<SnapshotCursor> {
  auto cursor = BTreeCursor::Seek(pages_, State().root_page_id, key);
  if (!cursor) {
    return std::unexpected(std::move(cursor).error());
  }
  // Copying snapshot_ ties cursor lifetime to this same reader admission.
  return SnapshotCursor(snapshot_, std::move(*cursor));
}

}  // namespace tinydb::txn
