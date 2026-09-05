#pragma once

#include "tinydb/storage/page.h"

namespace tinydb {

inline constexpr std::size_t MAX_ENTRY_SIZE = storage::PAGE_SIZE / 4;

}
