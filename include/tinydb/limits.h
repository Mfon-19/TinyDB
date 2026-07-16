#pragma once

#include <tinydb/page.h>

#include <cstddef>

namespace tinydb {

// Temporary page-derived cap: it guarantees that any overflowing node has a
// legal byte-balanced split. Overflow pages will eventually make the public
// value contract independent of page geometry.
constexpr std::size_t MAX_ENTRY_BYTES = PAGE_SIZE / 4 - 32;

}  // namespace tinydb
