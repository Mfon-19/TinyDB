#pragma once

#include <tinydb/file_header.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Private checksum helpers, shared by the WAL's record framing and the file
// header. This lives under src/ on purpose: it is not part of TinyDB's
// public API.

namespace tinydb {

// CRC-32 (the reflected IEEE polynomial — the same function as zlib's
// crc32), table-driven, with the table built at compile time.
constexpr auto MakeCrc32Table() -> std::array<std::uint32_t, 256> {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t byte = 0; byte < table.size(); ++byte) {
    std::uint32_t remainder = byte;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 1U) != 0 ? 0xEDB88320U ^ (remainder >> 1U) : remainder >> 1U;
    }
    table[byte] = remainder;
  }
  return table;
}
inline constexpr auto CRC32_TABLE = MakeCrc32Table();

inline auto Crc32(const char *data, std::size_t size) -> std::uint32_t {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    const auto byte = static_cast<unsigned char>(data[i]);
    crc = CRC32_TABLE[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

// The header's checksum covers every field before it in the struct; the
// field itself (and the struct's tail padding) is excluded.
inline auto HeaderChecksum(const FileHeader &header) -> std::uint32_t {
  constexpr std::size_t covered = offsetof(FileHeader, checksum);
  auto bytes = std::array<char, covered>{};
  std::memcpy(bytes.data(), &header, covered);
  return Crc32(bytes.data(), covered);
}

}  // namespace tinydb
