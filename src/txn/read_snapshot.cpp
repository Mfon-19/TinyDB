#include "txn/read_snapshot.h"

#include "btree/navigation.h"
#include "btree/page_source.h"
#include "btree/page_view.h"
#include "btree/value_storage.h"

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
  const auto leaf = LeafPageView::Open(*page);
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  const auto value = leaf->Get(key);
  if (!value) {
    return std::nullopt;
  }
  auto copied = tinydb::CopyValue(pages_, *value);
  if (!copied) {
    return std::unexpected(copied.error());
  }
  return std::optional<std::string>{std::move(*copied)};
}

auto SnapshotCursor::CopyValue() const -> Result<std::string> { return tinydb::CopyValue(pages_, cursor_.Value()); }

auto SnapshotCursor::First() -> Status {
  auto cursor = BTreeCursor::First(pages_, root_page_id_);
  if (!cursor) {
    return cursor.error();
  }
  cursor_ = std::move(*cursor);
  return {};
}

auto SnapshotCursor::Seek(std::string_view key) -> Status {
  auto cursor = BTreeCursor::Seek(pages_, root_page_id_, key);
  if (!cursor) {
    return cursor.error();
  }
  cursor_ = std::move(*cursor);
  return {};
}

auto ReadSnapshot::First() -> Result<SnapshotCursor> {
  auto cursor = BTreeCursor::First(pages_, State().root_page_id);
  if (!cursor) {
    return std::unexpected(std::move(cursor).error());
  }
  return SnapshotCursor(snapshot_, pages_, State().root_page_id, std::move(*cursor));
}

auto ReadSnapshot::Seek(std::string_view key) -> Result<SnapshotCursor> {
  auto cursor = BTreeCursor::Seek(pages_, State().root_page_id, key);
  if (!cursor) {
    return std::unexpected(std::move(cursor).error());
  }
  // Copying snapshot_ ties cursor lifetime to this same reader admission.
  return SnapshotCursor(snapshot_, pages_, State().root_page_id, std::move(*cursor));
}

}  // namespace tinydb::txn
