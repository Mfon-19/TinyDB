#pragma once

#include <cstddef>
#include <cstdint>

namespace tinydb {

using page_id_t = std::uint64_t;

// TinyDB stores the database file as fixed-size pages. Page 0 is reserved for
// file metadata; allocatable data pages start at page 1.
constexpr std::size_t PAGE_SIZE = 4096;
constexpr page_id_t HEADER_PAGE_ID = 0;
constexpr page_id_t FIRST_DATA_PAGE_ID = 1;
}  // namespace tinydb
