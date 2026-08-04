#include "txn/read_snapshot.h"

#include "btree/b_plus_tree.h"

#include <expected>
#include <utility>

namespace tinydb::txn {

/*
** Begin captures admission before exposing the wrapper. PageReader itself is
** not snapshot-versioned; the gate prevents publication from replacing cache
** versions while this snapshot or a cursor derived from it remains alive.
*/
auto ReadSnapshot::Begin(ReaderGate *gate, PageReader *pages) -> ReadSnapshot { return {gate->BeginRead(), pages}; }

auto ReadSnapshot::Get(std::string_view key) -> Result<std::optional<std::string>> {
  return BPlusTree::Read(pages_, State().root_page_id, key);
}

auto SnapshotCursor::CopyValue() const -> Result<std::string> { return cursor_.CopyValue(); }

auto SnapshotCursor::First() -> Status { return cursor_.First(); }

auto SnapshotCursor::Seek(std::string_view key) -> Status { return cursor_.Seek(key); }

auto ReadSnapshot::First() -> Result<SnapshotCursor> {
  auto cursor = BTreeCursor::First(pages_, State().root_page_id, State().logical_page_count);
  if (!cursor) {
    return std::unexpected(std::move(cursor).error());
  }
  return SnapshotCursor(snapshot_, std::move(*cursor));
}

auto ReadSnapshot::Seek(std::string_view key) -> Result<SnapshotCursor> {
  auto cursor = BTreeCursor::Seek(pages_, State().root_page_id, State().logical_page_count, key);
  if (!cursor) {
    return std::unexpected(std::move(cursor).error());
  }
  // Copying snapshot_ ties cursor lifetime to this same reader admission.
  return SnapshotCursor(snapshot_, std::move(*cursor));
}

}  // namespace tinydb::txn
