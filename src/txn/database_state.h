#pragma once

#include "storage/page.h"

#include <cstdint>

namespace tinydb::txn {

/*
** One immutable description of the visible committed database.
**
** Readers capture a shared instance at admission and use its root for every
** lookup. Publication replaces the complete instance. It never changes fields
** that an existing reader can observe. The roots locate the visible tree and
** free-page index.
**
** logical_page_count is the number of page slots in this database state. The
** count includes both superblocks and reusable data-page slots. A data page ID
** must be at least FIRST_DATA_PAGE_ID and less than this count. Allocating a
** new page increases the count. Reusing a page does not.
**
** visible_lsn identifies the newest visible transaction. checkpoint_lsn
** identifies the newest transaction stored in the database file, not only in
** the WAL.
**
** Required ordering:
**
**   checkpoint_lsn <= visible_lsn
**
** A page retired after checkpoint_lsn is not yet reusable. Recovery can still
** need an older physical image that only the WAL contains.
*/
struct DatabaseState final {
  page_id_t root_page_id{HEADER_PAGE_ID};            // visible B+ tree root
  page_id_t allocator_root_page_id{HEADER_PAGE_ID};  // free-extent chain root
  page_id_t logical_page_count{FIRST_DATA_PAGE_ID};  // number of logical page slots
  std::uint64_t visible_lsn{0};                      // newest published WAL LSN
  std::uint64_t checkpoint_lsn{0};                   // newest DB-file-resident LSN

  auto operator==(const DatabaseState &) const -> bool = default;
};

}  // namespace tinydb::txn
