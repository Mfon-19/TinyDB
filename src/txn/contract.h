#pragma once

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tinydb::txn {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);

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
  // B+ tree probes overwhelmingly use short fixed-width keys. Avoid an
  // out-of-line libc call for those comparisons while still letting memcmp's
  // vectorized implementation handle long byte strings.
  if (common_bytes <= 32) {
    auto offset = std::size_t{0};
    while (common_bytes - offset >= sizeof(std::uint64_t)) {
      auto left_word = std::uint64_t{0};
      auto right_word = std::uint64_t{0};
      std::memcpy(&left_word, left.data() + offset, sizeof(left_word));
      std::memcpy(&right_word, right.data() + offset, sizeof(right_word));
      if (left_word != right_word) {
        if constexpr (std::endian::native == std::endian::little) {
          left_word = std::byteswap(left_word);
          right_word = std::byteswap(right_word);
        }
        return static_cast<int>(left_word > right_word) - static_cast<int>(left_word < right_word);
      }
      offset += sizeof(std::uint64_t);
    }
    while (offset < common_bytes) {
      const auto left_byte = static_cast<unsigned char>(left[offset]);
      const auto right_byte = static_cast<unsigned char>(right[offset]);
      if (left_byte != right_byte) {
        return static_cast<int>(left_byte > right_byte) - static_cast<int>(left_byte < right_byte);
      }
      ++offset;
    }
  } else if (common_bytes != 0) {
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
