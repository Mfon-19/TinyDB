#pragma once

#include <cstddef>
#include <cstdint>

namespace tinydb {

using page_id_t = std::uint64_t;

/*
** The database file is divided into fixed 4096-byte pages, with page N
** beginning at byte N*PAGE_SIZE.  Pages 0 and 1 are alternating superblocks;
** data pages begin at page 2.
**
** Page number 0 is also the null link for leaves, allocator chains, and
** overflow chains, which is unambiguous because no data page can have number
** 0.
**
** Each data-page header stores its own page number.  A decoder compares that
** number with the physical position supplied by its caller, so a valid page
** written to the wrong offset is reported as corruption rather than being
** used under the wrong identity.
*/
constexpr std::size_t PAGE_SIZE = 4096;
constexpr page_id_t HEADER_PAGE_ID = 0;
constexpr page_id_t SUPERBLOCK_A_PAGE_ID = 0;
constexpr page_id_t SUPERBLOCK_B_PAGE_ID = 1;
constexpr page_id_t FIRST_DATA_PAGE_ID = 2;
}  // namespace tinydb
