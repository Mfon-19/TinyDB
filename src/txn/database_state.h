#pragma once

#include "storage/page.h"

#include <cstdint>

namespace tinydb::txn {

/*
** One immutable description of the visible committed database.
**
** Readers capture a shared instance at admission and use its root for every
** lookup. Publication replaces the complete instance; it never changes fields
** in an instance an existing reader can observe. The roots and high-water ID
** describe logical ownership. visible_lsn identifies the newest visible
** transaction, while checkpoint_lsn identifies the prefix already represented
** durably in the database file rather than only in WAL.
**
** Required ordering:
**
**   checkpoint_lsn <= visible_lsn
**
** A page retired after checkpoint_lsn is not yet reusable, because recovery
** may still need an older physical image covered only by WAL.
*/
struct DatabaseState final {
  page_id_t root_page_id{HEADER_PAGE_ID};            // visible B+ tree root
  page_id_t allocator_root_page_id{HEADER_PAGE_ID};  // free-extent chain root
  page_id_t high_water_page_id{FIRST_DATA_PAGE_ID};  // first never-allocated ID
  std::uint64_t transaction_id{0};                   // logical commit identity
  std::uint64_t visible_lsn{0};                      // newest published WAL LSN
  std::uint64_t checkpoint_lsn{0};                   // newest DB-file-resident LSN

  auto operator==(const DatabaseState &) const -> bool = default;
};

}  // namespace tinydb::txn
