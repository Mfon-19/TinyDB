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

class Crc32Accumulator final {
 public:
  void Update(std::span<const std::byte> data) noexcept {
    for (const auto value : data) {
      const auto byte = std::to_integer<unsigned int>(value);
      remainder_ = CRC32_TABLE[(remainder_ ^ byte) & 0xFFU] ^ (remainder_ >> 8U);
    }
  }

  auto Finish() const noexcept -> std::uint32_t { return remainder_ ^ 0xFFFFFFFFU; }

 private:
  std::uint32_t remainder_{0xFFFFFFFFU};
};

// Keeping the implementation byte-oriented makes the result identical across
// host endian and alignment rules. Codecs zero their checksum field before
// calling this function, then store the returned value little-endian.
inline auto Crc32(std::span<const std::byte> data) noexcept -> std::uint32_t {
  auto accumulator = Crc32Accumulator{};
  accumulator.Update(data);
  return accumulator.Finish();
}

inline auto Crc32(const char *data, std::size_t size) noexcept -> std::uint32_t {
  return Crc32(std::as_bytes(std::span{data, size}));
}

}  // namespace tinydb
