#pragma once

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include <algorithm>
#include <cstddef>
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

struct BytewiseLess {
  using is_transparent = void;

  constexpr auto operator()(std::string_view left, std::string_view right) const noexcept -> bool {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), [](char lhs, char rhs) {
      return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
    });
  }
};

}  // namespace tinydb::txn
