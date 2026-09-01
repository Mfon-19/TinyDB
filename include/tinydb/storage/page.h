#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tinydb::storage {

inline constexpr std::size_t PAGE_SIZE = 4096;

using PageId = std::uint32_t;
inline constexpr PageId INVALID_PAGE_ID = UINT32_MAX;
using PageBytes = std::array<char, PAGE_SIZE>;

} // namespace tinydb::storage
