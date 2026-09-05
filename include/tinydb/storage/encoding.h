#pragma once

/*
 * Explicit little-endian encoding
 */

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace tinydb::storage::little_endian {

static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big);

inline auto GetU16(std::span<const char> bytes,
                   std::size_t offset) noexcept -> std::uint16_t {
  assert(bytes.size() >= sizeof(std::uint16_t));
  assert(offset <= bytes.size() - sizeof(std::uint16_t));

  std::uint16_t value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return std::endian::native == std::endian::little ? value
                                                    : std::byteswap(value);
}

inline auto GetU32(std::span<const char> bytes,
                   std::size_t offset) noexcept -> std::uint32_t {
  assert(bytes.size() >= sizeof(std::uint32_t));
  assert(offset <= bytes.size() - sizeof(std::uint32_t));

  std::uint32_t value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return std::endian::native == std::endian::little ? value
                                                    : std::byteswap(value);
}

inline void PutU16(std::span<char> bytes, std::size_t offset,
                   std::uint16_t value) noexcept {
  assert(bytes.size() >= sizeof(value));
  assert(offset <= bytes.size() - sizeof(value));

  bytes[offset] = static_cast<char>(value & 0xFFU);
  bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xFFU);
}

inline void PutU32(std::span<char> bytes, std::size_t offset,
                   std::uint32_t value) noexcept {
  assert(bytes.size() >= sizeof(value));
  assert(offset <= bytes.size() - sizeof(value));

  bytes[offset] = static_cast<char>(value & 0xFFU);
  bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xFFU);
  bytes[offset + 2] = static_cast<char>((value >> 16U) & 0xFFU);
  bytes[offset + 3] = static_cast<char>((value >> 24U) & 0xFFU);
}

} // namespace tinydb::storage::little_endian
