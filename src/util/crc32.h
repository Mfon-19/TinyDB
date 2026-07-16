#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb {

constexpr auto MakeCrc32Table() -> std::array<std::uint32_t, 256> {
  auto table = std::array<std::uint32_t, 256>{};
  for (std::uint32_t byte = 0; byte < table.size(); ++byte) {
    auto remainder = byte;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 1U) != 0 ? 0xEDB88320U ^ (remainder >> 1U) : remainder >> 1U;
    }
    table[byte] = remainder;
  }
  return table;
}

inline constexpr auto CRC32_TABLE = MakeCrc32Table();

inline auto Crc32(std::span<const std::byte> data) noexcept -> std::uint32_t {
  auto crc = std::uint32_t{0xFFFFFFFFU};
  for (const auto value : data) {
    const auto byte = std::to_integer<unsigned int>(value);
    crc = CRC32_TABLE[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

inline auto Crc32(const char *data, std::size_t size) noexcept -> std::uint32_t {
  return Crc32(std::as_bytes(std::span{data, size}));
}

}  // namespace tinydb
