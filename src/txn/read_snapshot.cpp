#include "txn/read_snapshot.h"

#include "btree/b_plus_tree.h"

namespace tinydb::txn {

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
