#pragma once

/*
 * Explicit little-endian encoding
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::storage::little_endian {

inline auto GetU16(std::span<const char> bytes,
                   std::size_t offset) noexcept -> std::uint16_t {
  assert(bytes.size() >= sizeof(std::uint16_t));
  assert(offset <= bytes.size() - sizeof(std::uint16_t));

  const auto byte0 = static_cast<unsigned char>(bytes[offset]);
  const auto byte1 = static_cast<unsigned char>(bytes[offset + 1]);
  return static_cast<std::uint16_t>(byte0) |
         static_cast<std::uint16_t>(byte1 << 8U);
}

inline auto GetU32(std::span<const char> bytes,
                   std::size_t offset) noexcept -> std::uint32_t {
  assert(bytes.size() >= sizeof(std::uint32_t));
  assert(offset <= bytes.size() - sizeof(std::uint32_t));

  const auto byte0 = static_cast<unsigned char>(bytes[offset]);
  const auto byte1 = static_cast<unsigned char>(bytes[offset + 1]);
  const auto byte2 = static_cast<unsigned char>(bytes[offset + 2]);
  const auto byte3 = static_cast<unsigned char>(bytes[offset + 3]);
  return static_cast<std::uint32_t>(byte0) |
         (static_cast<std::uint32_t>(byte1) << 8U) |
         (static_cast<std::uint32_t>(byte2) << 16U) |
         (static_cast<std::uint32_t>(byte3) << 24U);
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
