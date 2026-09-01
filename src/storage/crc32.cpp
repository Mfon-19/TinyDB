#include "tinydb/storage/crc32.h"

namespace tinydb::storage {

namespace {

inline constexpr std::uint32_t CRC32_POLYNOMIAL = 0xEDB88320U;

} // namespace

void Crc32Accumulator::Update(std::span<const char> bytes) noexcept {
  for (const char byte : bytes) {
    remainder_ ^= static_cast<unsigned char>(byte);
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((remainder_ & 1U) != 0) {
        remainder_ = (remainder_ >> 1U) ^ CRC32_POLYNOMIAL;
      } else {
        remainder_ >>= 1U;
      }
    }
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

} // namespace tinydb::storage
