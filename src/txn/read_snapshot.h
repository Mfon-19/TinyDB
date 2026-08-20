#pragma once

#include "btree/tree_cursor.h"
#include "txn/reader_gate.h"

#include <tinydb/status.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tinydb {
class PageReader;
}

namespace tinydb::txn {

/*
** SNAPSHOT READS
**
** A ReadSnapshot combines one ReaderGate admission with the committed page
** reader. All operations start at the root captured in that admission. An
** escaping cursor's owner keeps a Token copy declared before the cursor, so
** the cursor's page lease is released before the final reader admission and
** the cursor may outlive the transaction that created it.
**
** PageReader is owned by DatabaseCore. Public ReadTransaction holds this
** object for its full lifetime. Returned point values are owning copies.
** Cursor keys borrow the current page. CopyValue returns owned bytes and
** reports any overflow-page I/O or corruption through Result.
*/
class ReadSnapshot final {
 public:
  static auto Begin(ReaderGate *gate, PageReader *pages) -> ReadSnapshot;

  ReadSnapshot(const ReadSnapshot &) = delete;
  auto operator=(const ReadSnapshot &) -> ReadSnapshot & = delete;
  ReadSnapshot(ReadSnapshot &&) noexcept = default;
  auto operator=(ReadSnapshot &&) -> ReadSnapshot & = delete;

  auto State() const -> const DatabaseState & { return snapshot_.State(); }
  // A copy shares this snapshot's single reader admission with a cursor that
  // escapes the transaction.
  auto Token() const -> SnapshotToken { return snapshot_; }
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto First() -> Result<BTreeCursor>;
  auto Seek(std::string_view key) -> Result<BTreeCursor>;

 private:
  ReadSnapshot(SnapshotToken snapshot, PageReader *pages) : snapshot_(std::move(snapshot)), pages_(pages) {}

  SnapshotToken snapshot_;
  PageReader *pages_;
};

}  // namespace tinydb::txn
