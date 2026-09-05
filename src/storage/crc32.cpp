#include "tinydb/storage/crc32.h"
#include "tinydb/storage/encoding.h"
#include <array>
#include <cassert>

namespace tinydb::storage {

namespace {

inline constexpr std::uint32_t CRC32_POLYNOMIAL = 0xEDB88320U;

constexpr auto CRC32_TABLE = [] {
  std::array<std::array<std::uint32_t, 256>, 8> table{};
  for (std::uint32_t value = 0; value < table[0].size(); ++value) {
    auto remainder = value;
    for (unsigned bit = 0; bit < 8; ++bit) {
      remainder = (remainder >> 1U) ^
                  ((remainder & 1U) != 0 ? CRC32_POLYNOMIAL : 0U);
    }
    table[0][value] = remainder;
  }
  for (std::size_t slice = 1; slice < table.size(); ++slice) {
    for (std::size_t value = 0; value < table[slice].size(); ++value) {
      const auto remainder = table[slice - 1][value];
      table[slice][value] =
          (remainder >> 8U) ^ table[0][remainder & 0xFFU];
    }
  }
  return table;
}();

} // namespace

void Crc32Accumulator::Update(std::span<const char> bytes) noexcept {
  while (bytes.size() >= 8) {
    const auto low = little_endian::GetU32(bytes, 0) ^ remainder_;
    const auto high = little_endian::GetU32(bytes, 4);
    remainder_ = CRC32_TABLE[7][low & 0xFFU] ^
                 CRC32_TABLE[6][(low >> 8U) & 0xFFU] ^
                 CRC32_TABLE[5][(low >> 16U) & 0xFFU] ^
                 CRC32_TABLE[4][low >> 24U] ^
                 CRC32_TABLE[3][high & 0xFFU] ^
                 CRC32_TABLE[2][(high >> 8U) & 0xFFU] ^
                 CRC32_TABLE[1][(high >> 16U) & 0xFFU] ^
                 CRC32_TABLE[0][high >> 24U];
    bytes = bytes.subspan(8);
  }
  for (const char byte : bytes) {
    remainder_ = (remainder_ >> 8U) ^
                 CRC32_TABLE[0][(remainder_ ^ static_cast<unsigned char>(byte)) &
                                0xFFU];
  }
}

auto Crc32Accumulator::Finish() const noexcept -> std::uint32_t {
  return remainder_ ^ 0xFFFFFFFFU;
}

auto Crc32(std::span<const char> bytes) noexcept -> std::uint32_t {
  Crc32Accumulator crc;
  crc.Update(bytes);
  return crc.Finish();
}

auto Crc32WithZeroedU32(std::span<const char> bytes,
                        std::size_t offset) noexcept -> std::uint32_t {
  constexpr std::array<char, sizeof(std::uint32_t)> zeros{};
  assert(bytes.size() >= zeros.size());
  assert(offset <= bytes.size() - zeros.size());

  Crc32Accumulator crc;
  crc.Update(bytes.first(offset));
  crc.Update(zeros);
  crc.Update(bytes.subspan(offset + zeros.size()));
  return crc.Finish();
}

} // namespace tinydb::storage
