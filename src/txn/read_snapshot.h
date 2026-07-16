#pragma once

#include "btree/tree_cursor.h"
#include "txn/reader_gate.h"

#include <tinydb/status.h>

#include <optional>
#include <string>
#include <string_view>

namespace tinydb {
class PageReader;
}

namespace tinydb::txn {

// Cursor over one admitted committed state. The snapshot token is declared
// before the tree cursor so the page lease is destroyed before reader
// admission is released.
class SnapshotCursor final {
 public:
  SnapshotCursor(const SnapshotCursor &) = delete;
  auto operator=(const SnapshotCursor &) -> SnapshotCursor & = delete;
  SnapshotCursor(SnapshotCursor &&) noexcept = default;
  auto operator=(SnapshotCursor &&) noexcept -> SnapshotCursor & = default;

  auto Valid() const -> bool { return cursor_.Valid(); }
  auto Key() const -> std::string_view { return cursor_.Key(); }
  auto Value() const -> std::string_view { return cursor_.Value(); }
  auto Next() -> Status { return cursor_.Next(); }

 private:
  SnapshotCursor(SnapshotToken snapshot, BTreeCursor cursor)
      : snapshot_(std::move(snapshot)), cursor_(std::move(cursor)) {}

  SnapshotToken snapshot_;
  BTreeCursor cursor_;

  friend class ReadSnapshot;
};

// Internal read-transaction core. PageReader is owned by the database and
// remains alive while this token prevents Close; the captured DatabaseState
// supplies the root for every operation in the snapshot.
class ReadSnapshot final {
 public:
  static auto Begin(ReaderGate *gate, PageReader *pages) -> ReadSnapshot;

  ReadSnapshot(const ReadSnapshot &) = delete;
  auto operator=(const ReadSnapshot &) -> ReadSnapshot & = delete;
  ReadSnapshot(ReadSnapshot &&) noexcept = default;
  auto operator=(ReadSnapshot &&) noexcept -> ReadSnapshot & = default;

  auto State() const -> const DatabaseState & { return snapshot_.State(); }
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Seek(std::string_view key) -> Result<SnapshotCursor>;

 private:
  ReadSnapshot(SnapshotToken snapshot, PageReader *pages) : snapshot_(std::move(snapshot)), pages_(pages) {}

  SnapshotToken snapshot_;
  PageReader *pages_;
};

}  // namespace tinydb::txn
