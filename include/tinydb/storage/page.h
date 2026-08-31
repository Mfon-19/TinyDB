#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tinydb::storage {

inline constexpr std::size_t PAGE_SIZE = 4096;

using PageId = std::uint32_t;
using PageBytes = std::array<char, PAGE_SIZE>;

} // namespace tinydb::storage
