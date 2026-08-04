#pragma once

#include "util/check.h"

#include <array>
#include <bit>
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

constexpr auto MakeCrc32SlicingTable() -> std::array<std::array<std::uint32_t, 256>, 16> {
  auto tables = std::array<std::array<std::uint32_t, 256>, 16>{};
  tables[0] = MakeCrc32Table();
  for (std::size_t slice = 1; slice < tables.size(); ++slice) {
    for (std::size_t byte = 0; byte < tables[slice].size(); ++byte) {
      const auto previous = tables[slice - 1][byte];
      tables[slice][byte] = tables[0][previous & 0xFFU] ^ (previous >> 8U);
    }
  }
  return tables;
}

inline constexpr auto CRC32_SLICING_TABLE = MakeCrc32SlicingTable();

namespace crc32_detail {

using Matrix = std::array<std::uint32_t, 32>;

constexpr auto MatrixTimes(const Matrix &matrix, std::uint32_t vector) noexcept -> std::uint32_t {
  auto result = std::uint32_t{0};
  while (vector != 0) {
    result ^= matrix[static_cast<std::size_t>(std::countr_zero(vector))];
    vector &= vector - 1U;
  }
  return result;
}

constexpr auto MatrixSquare(const Matrix &matrix) noexcept -> Matrix {
  auto square = Matrix{};
  for (auto index = std::size_t{0}; index < square.size(); ++index) {
    square[index] = MatrixTimes(matrix, matrix[index]);
  }
  return square;
}

constexpr auto MakeByteShiftMatrices() noexcept -> std::array<Matrix, 64> {
  auto one_bit = Matrix{};
  one_bit[0] = 0xEDB88320U;
  auto row = std::uint32_t{1};
  for (auto index = std::size_t{1}; index < one_bit.size(); ++index) {
    one_bit[index] = row;
    row <<= 1U;
  }

  const auto two_bits = MatrixSquare(one_bit);
  const auto four_bits = MatrixSquare(two_bits);
  auto matrices = std::array<Matrix, 64>{};
  matrices[0] = MatrixSquare(four_bits);
  for (auto index = std::size_t{1}; index < matrices.size(); ++index) {
    matrices[index] = MatrixSquare(matrices[index - 1]);
  }
  return matrices;
}

inline constexpr auto BYTE_SHIFT_MATRICES = MakeByteShiftMatrices();

inline auto Shift(std::uint32_t crc, std::size_t zero_bytes) noexcept -> std::uint32_t {
  auto power = std::size_t{0};
  while (zero_bytes != 0) {
    if ((zero_bytes & 1U) != 0) {
      crc = MatrixTimes(BYTE_SHIFT_MATRICES[power], crc);
    }
    zero_bytes >>= 1U;
    ++power;
  }
  return crc;
}

}  // namespace crc32_detail

class Crc32Accumulator final {
 public:
  void Update(std::span<const std::byte> data) noexcept {
    // Slicing by 16 keeps every lookup independent and halves loop/control
    // overhead while retaining a portable IEEE CRC-32 implementation.
    while (data.size() >= 16) {
      const auto first = LoadLittleEndian(data) ^ remainder_;
      const auto second = LoadLittleEndian(data.subspan(4));
      const auto third = LoadLittleEndian(data.subspan(8));
      const auto fourth = LoadLittleEndian(data.subspan(12));
      remainder_ = CRC32_SLICING_TABLE[15][first & 0xFFU] ^ CRC32_SLICING_TABLE[14][(first >> 8U) & 0xFFU] ^
                   CRC32_SLICING_TABLE[13][(first >> 16U) & 0xFFU] ^ CRC32_SLICING_TABLE[12][first >> 24U] ^
                   CRC32_SLICING_TABLE[11][second & 0xFFU] ^ CRC32_SLICING_TABLE[10][(second >> 8U) & 0xFFU] ^
                   CRC32_SLICING_TABLE[9][(second >> 16U) & 0xFFU] ^ CRC32_SLICING_TABLE[8][second >> 24U] ^
                   CRC32_SLICING_TABLE[7][third & 0xFFU] ^ CRC32_SLICING_TABLE[6][(third >> 8U) & 0xFFU] ^
                   CRC32_SLICING_TABLE[5][(third >> 16U) & 0xFFU] ^ CRC32_SLICING_TABLE[4][third >> 24U] ^
                   CRC32_SLICING_TABLE[3][fourth & 0xFFU] ^ CRC32_SLICING_TABLE[2][(fourth >> 8U) & 0xFFU] ^
                   CRC32_SLICING_TABLE[1][(fourth >> 16U) & 0xFFU] ^ CRC32_SLICING_TABLE[0][fourth >> 24U];
      data = data.subspan(16);
    }
    for (const auto value : data) {
      const auto byte = std::to_integer<unsigned int>(value);
      remainder_ = CRC32_SLICING_TABLE[0][(remainder_ ^ byte) & 0xFFU] ^ (remainder_ >> 8U);
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

// Calculate a CRC after treating one four-byte field as zero. The input bytes
// remain unchanged.
inline auto Crc32WithZeroedU32(std::span<const std::byte> data, std::size_t offset) noexcept -> std::uint32_t {
  constexpr auto zero_field = std::array<std::byte, sizeof(std::uint32_t)>{};
  auto accumulator = Crc32Accumulator{};
  accumulator.Update(data.first(offset));
  accumulator.Update(zero_field);
  accumulator.Update(data.subspan(offset + zero_field.size()));
  return accumulator.Finish();
}

// Combine already-computed checksums without rereading either byte range.
inline auto Crc32Combine(std::uint32_t prefix_crc, std::uint32_t suffix_crc,
                         std::size_t suffix_bytes) noexcept -> std::uint32_t {
  return crc32_detail::Shift(prefix_crc, suffix_bytes) ^ suffix_crc;
}

// Update a complete checksum after replacing one equal-length byte range. The
// caller supplies the number of bytes following the replaced range.
inline auto Crc32Replace(std::uint32_t original_crc, std::span<const std::byte> original,
                         std::span<const std::byte> replacement, std::size_t trailing_bytes) noexcept -> std::uint32_t {
  TINYDB_CHECK(original.size() == replacement.size(), "CRC replacement changed byte length");
  const auto difference = Crc32(original) ^ Crc32(replacement);
  return original_crc ^ crc32_detail::Shift(difference, trailing_bytes);
}

}  // namespace tinydb
