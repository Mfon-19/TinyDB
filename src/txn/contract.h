#pragma once

#include <tinydb/status.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace tinydb::txn {

inline constexpr std::size_t MAX_KEY_BYTES = 1024;

constexpr auto ValidateKeySize(std::size_t size) noexcept -> StatusCode {
  return size <= MAX_KEY_BYTES ? StatusCode::Ok : StatusCode::InvalidArgument;
}

// std::string ordering is not the persistent contract. Spell out unsigned
// bytewise ordering so keys sort identically even where char is signed.
struct BytewiseLess {
  using is_transparent = void;

  constexpr auto operator()(std::string_view left, std::string_view right) const noexcept -> bool {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), [](char lhs, char rhs) {
      return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
    });
  }
};

}  // namespace tinydb::txn
