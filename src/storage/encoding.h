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
** order, enum representation, or compiler ABI details.
**
** Every helper is bounds-checked and returns bool or optional rather than
** asserting. Decode offsets ultimately address untrusted files, so malformed
** bytes must become Corruption at the owning codec boundary instead of
** terminating the host application. Encoders use the same checks so a future
** layout that no longer fits its buffer fails visibly during development.
*/
template <typename Integer>
  requires std::is_unsigned_v<Integer>
constexpr auto PutLittleEndian(std::span<std::byte> output, std::size_t offset, Integer value) noexcept -> bool {
  // The subtraction form avoids overflow in `offset + sizeof(Integer)` when
  // offset itself came from a corrupt or otherwise untrusted source.
  if (offset > output.size() || output.size() - offset < sizeof(Integer)) {
    return false;
  }
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    output[offset + i] = static_cast<std::byte>(value & static_cast<Integer>(0xFFU));
    value >>= 8U;
  }
  return true;
}

template <typename Integer>
  requires std::is_unsigned_v<Integer>
constexpr auto GetLittleEndian(std::span<const std::byte> input,
                               std::size_t offset) noexcept -> std::optional<Integer> {
  // Accumulate in the widest supported scratch type so shifting a small
  // Integer (for example uint16_t) never promotes through signed int.
  static_assert(sizeof(Integer) <= sizeof(std::uint64_t));
  if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
    return std::nullopt;
  }
  auto value = std::uint64_t{0};
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(input[offset + i])) << (i * 8U);
  }
  return static_cast<Integer>(value);
}

constexpr auto PutBytes(std::span<std::byte> output, std::size_t offset,
                        std::span<const std::byte> value) noexcept -> bool {
  // Byte strings such as magic values and UUIDs have no endian conversion;
  // they are copied exactly as the file-format specification spells them.
  if (offset > output.size() || output.size() - offset < value.size()) {
    return false;
  }
  std::ranges::copy(value, output.begin() + static_cast<std::ptrdiff_t>(offset));
  return true;
}

constexpr auto GetBytes(std::span<const std::byte> input, std::size_t offset,
                        std::span<std::byte> output) noexcept -> bool {
  if (offset > input.size() || input.size() - offset < output.size()) {
    return false;
  }
  std::ranges::copy(input.subspan(offset, output.size()), output.begin());
  return true;
}

}  // namespace tinydb::storage
