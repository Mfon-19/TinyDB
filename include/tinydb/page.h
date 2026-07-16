#pragma once

#include <cstddef>
#include <cstdint>

namespace tinydb {

using page_id_t = std::uint64_t;

// The database file is an array of fixed-size pages. Pages 0 and 1 are the
// alternating superblocks; data starts at page 2. Page id 0 also serves as
// the null reference in tree and allocator links because no data page may
// ever use it.
constexpr std::size_t PAGE_SIZE = 4096;
constexpr page_id_t HEADER_PAGE_ID = 0;
constexpr page_id_t SUPERBLOCK_A_PAGE_ID = 0;
constexpr page_id_t SUPERBLOCK_B_PAGE_ID = 1;
constexpr page_id_t FIRST_DATA_PAGE_ID = 2;
}  // namespace tinydb
