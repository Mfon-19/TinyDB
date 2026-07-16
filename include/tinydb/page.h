#pragma once

#include <cstddef>
#include <cstdint>

namespace tinydb {

using page_id_t = std::uint64_t;

/*
** The database file is an array of fixed 4096-byte pages. Pages 0 and 1 are
** alternating superblocks and data begins at page 2. Page ID zero also serves
** as the null link for leaves, allocator chains, and overflow chains because
** no data page may ever use it.
**
** Page IDs are physical: page N begins at byte N*PAGE_SIZE. Persisted data-page
** headers repeat this identity so codecs can detect a valid page written to an
** incorrect physical offset.
*/
constexpr std::size_t PAGE_SIZE = 4096;
constexpr page_id_t HEADER_PAGE_ID = 0;
constexpr page_id_t SUPERBLOCK_A_PAGE_ID = 0;
constexpr page_id_t SUPERBLOCK_B_PAGE_ID = 1;
constexpr page_id_t FIRST_DATA_PAGE_ID = 2;
}  // namespace tinydb
