#pragma once

#include <tinydb/bytes.h>
#include <tinydb/status.h>

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
constexpr auto ValidateKeySize(std::size_t size) noexcept -> StatusCode {
  return size <= tinydb::MAX_KEY_BYTES ? StatusCode::Ok : StatusCode::InvalidArgument;
}

constexpr auto ValidateValueSize(std::size_t size) noexcept -> StatusCode {
  return size <= MAX_VALUE_BYTES ? StatusCode::Ok : StatusCode::InvalidArgument;
}

inline auto BytewiseCompare(std::string_view left, std::string_view right) noexcept -> int {
  const auto common_bytes = std::min(left.size(), right.size());
  if (common_bytes != 0) {
    const auto order = std::memcmp(left.data(), right.data(), common_bytes);
    if (order != 0) {
      // memcmp compares bytes as unsigned char, exactly matching the
      // persistent key-order contract.
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
