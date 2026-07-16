#pragma once

#include <cstddef>

namespace tinydb {

/*
** Values larger than a leaf's inline budget are stored in overflow pages, so
** this application contract is independent of tree-page geometry. The limit
** bounds copying Get/CopyValue results and ensures one maximum-sized value can
** be prepared within the default write-transaction memory budget.
*/
constexpr std::size_t MAX_VALUE_BYTES = 4U << 20U;

}  // namespace tinydb
