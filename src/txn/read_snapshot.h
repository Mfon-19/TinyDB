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
  auto ValueSize() const -> std::uint64_t { return cursor_.Value().Size(); }
  auto CopyValue() const -> Result<std::string>;
  auto First() -> Status;
  auto Seek(std::string_view key) -> Status;
  auto Next() -> Status { return cursor_.Next(); }

 private:
  SnapshotCursor(SnapshotToken snapshot, PageReader *pages, page_id_t root_page_id, BTreeCursor cursor)
      : snapshot_(std::move(snapshot)), pages_(pages), root_page_id_(root_page_id), cursor_(std::move(cursor)) {}

  SnapshotToken snapshot_;
  PageReader *pages_;
  page_id_t root_page_id_;
  BTreeCursor cursor_;

  friend class ReadSnapshot;
};

/*
** Internal read-transaction core. PageReader is owned by DatabaseCore. Public
** ReadTransaction holds this object for its full lifetime, and convenience
** reads construct the same transaction temporarily. Returned point values are
** owning copies. Cursor keys borrow the current page; value copies cross a
** Result boundary now so overflow-chain I/O and corruption can use the same
** API later without changing public ownership semantics.
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
  auto First() -> Result<SnapshotCursor>;
  auto Seek(std::string_view key) -> Result<SnapshotCursor>;

 private:
  ReadSnapshot(SnapshotToken snapshot, PageReader *pages) : snapshot_(std::move(snapshot)), pages_(pages) {}

  SnapshotToken snapshot_;
  PageReader *pages_;
};

}  // namespace tinydb::txn
