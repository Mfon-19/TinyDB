#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace tinydb::storage {

/*
** PERSISTENT ENCODING PRIMITIVES
**
** These are the only primitives persistent codecs use for fixed-width fields.
** Encoding field-by-field avoids persisting C++ padding, alignment, native byte
** order, enum representation, or compiler ABI details. Use an Unchecked form
** only after a fixed span or an earlier size check proves that the field fits.
*/
template <typename Integer>
  requires std::is_unsigned_v<Integer>
constexpr auto PutLittleEndianUnchecked(std::span<std::byte> output, std::size_t offset,
                                        Integer value) noexcept -> void {
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    output[offset + i] = static_cast<std::byte>(value & static_cast<Integer>(0xFFU));
    value >>= 8U;
  }
}

template <typename Integer>
  requires std::is_unsigned_v<Integer>
constexpr auto PutLittleEndian(std::span<std::byte> output, std::size_t offset, Integer value) noexcept -> bool {
  if (offset > output.size() || output.size() - offset < sizeof(Integer)) {
    return false;
  }
  PutLittleEndianUnchecked(output, offset, value);
  return true;
}

template <typename Integer>
  requires std::is_unsigned_v<Integer>
constexpr auto GetLittleEndianUnchecked(std::span<const std::byte> input, std::size_t offset) noexcept -> Integer {
  static_assert(sizeof(Integer) <= sizeof(std::uint64_t));
  auto value = std::uint64_t{0};
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(input[offset + i])) << (i * 8U);
  }
  return static_cast<Integer>(value);
}

template <typename Integer>
  requires std::is_unsigned_v<Integer>
constexpr auto GetLittleEndian(std::span<const std::byte> input,
                               std::size_t offset) noexcept -> std::optional<Integer> {
  if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
    return std::nullopt;
  }
  return GetLittleEndianUnchecked<Integer>(input, offset);
}

constexpr auto PutBytesUnchecked(std::span<std::byte> output, std::size_t offset,
                                 std::span<const std::byte> value) noexcept -> void {
  std::ranges::copy(value, output.begin() + static_cast<std::ptrdiff_t>(offset));
}

constexpr auto GetBytesUnchecked(std::span<const std::byte> input, std::size_t offset,
                                 std::span<std::byte> output) noexcept -> void {
  std::ranges::copy(input.subspan(offset, output.size()), output.begin());
}

}  // namespace tinydb::storage
