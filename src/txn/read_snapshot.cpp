#include "txn/read_snapshot.h"

#include "btree/b_plus_tree.h"

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

auto ReadSnapshot::First() -> Result<BTreeCursor> {
  return BTreeCursor::First(pages_, State().root_page_id, State().logical_page_count);
}

auto ReadSnapshot::Seek(std::string_view key) -> Result<BTreeCursor> {
  return BTreeCursor::Seek(pages_, State().root_page_id, State().logical_page_count, key);
}

}  // namespace tinydb::txn
