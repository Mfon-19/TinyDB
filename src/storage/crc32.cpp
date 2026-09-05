#include "tinydb/storage/crc32.h"
#include <array>
#include <cassert>

namespace tinydb::storage {

namespace {

inline constexpr std::uint32_t CRC32_POLYNOMIAL = 0xEDB88320U;

constexpr auto CRC32_TABLE = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t value = 0; value < table.size(); ++value) {
    auto remainder = value;
    for (unsigned bit = 0; bit < 8; ++bit) {
      remainder = (remainder >> 1U) ^
                  ((remainder & 1U) != 0 ? CRC32_POLYNOMIAL : 0U);
    }
    table[value] = remainder;
  }
  return table;
}();

} // namespace

void Crc32Accumulator::Update(std::span<const char> bytes) noexcept {
  for (const char byte : bytes) {
    remainder_ = (remainder_ >> 8U) ^
                 CRC32_TABLE[(remainder_ ^ static_cast<unsigned char>(byte)) &
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
