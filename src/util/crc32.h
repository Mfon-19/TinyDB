#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb {

// IEEE CRC-32 (reflected polynomial 0xEDB88320), used as a corruption and
// torn-write detector for persistent pages and WAL frames. It is not a
// cryptographic authenticity check: an attacker who can rewrite the file can
// also recompute it.
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

constexpr auto MakeCrc32SlicingTable() -> std::array<std::array<std::uint32_t, 256>, 8> {
  auto tables = std::array<std::array<std::uint32_t, 256>, 8>{};
  tables[0] = CRC32_TABLE;
  for (std::size_t slice = 1; slice < tables.size(); ++slice) {
    for (std::size_t byte = 0; byte < tables[slice].size(); ++byte) {
      const auto previous = tables[slice - 1][byte];
      tables[slice][byte] = CRC32_TABLE[previous & 0xFFU] ^ (previous >> 8U);
    }
  }
  return tables;
}

inline constexpr auto CRC32_SLICING_TABLE = MakeCrc32SlicingTable();

class Crc32Accumulator final {
 public:
  void Update(std::span<const std::byte> data) noexcept {
    while (data.size() >= 8) {
      const auto first = LoadLittleEndian(data) ^ remainder_;
      const auto second = LoadLittleEndian(data.subspan(4));
      remainder_ = CRC32_SLICING_TABLE[7][first & 0xFFU] ^ CRC32_SLICING_TABLE[6][(first >> 8U) & 0xFFU] ^
                   CRC32_SLICING_TABLE[5][(first >> 16U) & 0xFFU] ^ CRC32_SLICING_TABLE[4][first >> 24U] ^
                   CRC32_SLICING_TABLE[3][second & 0xFFU] ^ CRC32_SLICING_TABLE[2][(second >> 8U) & 0xFFU] ^
                   CRC32_SLICING_TABLE[1][(second >> 16U) & 0xFFU] ^ CRC32_SLICING_TABLE[0][second >> 24U];
      data = data.subspan(8);
    }
    for (const auto value : data) {
      const auto byte = std::to_integer<unsigned int>(value);
      remainder_ = CRC32_TABLE[(remainder_ ^ byte) & 0xFFU] ^ (remainder_ >> 8U);
    }
  }

  auto Finish() const noexcept -> std::uint32_t { return remainder_ ^ 0xFFFFFFFFU; }

 private:
  static auto LoadLittleEndian(std::span<const std::byte> data) noexcept -> std::uint32_t {
    return std::to_integer<std::uint32_t>(data[0]) | (std::to_integer<std::uint32_t>(data[1]) << 8U) |
           (std::to_integer<std::uint32_t>(data[2]) << 16U) | (std::to_integer<std::uint32_t>(data[3]) << 24U);
  }

  std::uint32_t remainder_{0xFFFFFFFFU};
};

// Explicit little-endian loads keep the slicing path identical across host
// endian and alignment rules. Codecs store the returned value little-endian.
inline auto Crc32(std::span<const std::byte> data) noexcept -> std::uint32_t {
  auto accumulator = Crc32Accumulator{};
  accumulator.Update(data);
  return accumulator.Finish();
}

inline auto Crc32(const char *data, std::size_t size) noexcept -> std::uint32_t {
  return Crc32(std::as_bytes(std::span{data, size}));
}

}  // namespace tinydb
