#pragma once

#include <tinydb/bytes.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace tinydb::txn {

/*
** PERSISTENT KEY CONTRACT
**
** Keys are arbitrary byte strings. Their order must not depend on whether the
** platform's plain char is signed, because the same encoded B+ tree must route
** identically after reopening on every supported build. Empty keys are valid;
** the upper size bound limits separators copied into internal pages.
*/
constexpr auto ValidKeySize(std::size_t size) noexcept -> bool { return size <= tinydb::MAX_KEY_BYTES; }

constexpr auto ValidValueSize(std::size_t size) noexcept -> bool { return size <= MAX_VALUE_BYTES; }

inline auto BytewiseCompare(std::string_view left, std::string_view right) noexcept -> int {
  const auto common_bytes = std::min(left.size(), right.size());
  if (common_bytes != 0) {
    const auto order = std::memcmp(left.data(), right.data(), common_bytes);
    if (order != 0) {
      return order;
    }
  }
  return static_cast<int>(left.size() > right.size()) - static_cast<int>(left.size() < right.size());
}

struct BytewiseLess {
  using is_transparent = void;

  auto operator()(std::string_view left, std::string_view right) const noexcept -> bool {
    return BytewiseCompare(left, right) < 0;
  }
};

}  // namespace tinydb::txn
