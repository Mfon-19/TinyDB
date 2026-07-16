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

/*
** SNAPSHOT READS
**
** A ReadSnapshot combines one ReaderGate admission with the committed page
** reader. All operations start at the root captured in that admission. A
** SnapshotCursor copies the admission token, so it may outlive the wrapper
** that created it without allowing publication to invalidate its page bytes.
**
** Member destruction order is significant: snapshot_ is declared before the
** tree cursor, so the cursor's page lease is released before the final reader
** admission can be released.
*/
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

/*
** Internal read-transaction core. PageReader is owned by the database. The
** current compatibility facade completes snapshots inside one API call; the
** public transaction layer will count the admission when deciding whether
** Close is Busy. Returned point values are owning copies; cursor keys and
** values remain borrowed from its current page.
*/
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
