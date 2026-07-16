#pragma once

#include <tinydb/file_header.h>

#include "util/crc32.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Private checksum helpers, shared by the WAL's record framing and the file
// header. This lives under src/ on purpose: it is not part of TinyDB's
// public API.

namespace tinydb {

// The header's checksum covers every field before it in the struct; the
// field itself (and the struct's tail padding) is excluded.
inline auto HeaderChecksum(const FileHeader &header) -> std::uint32_t {
  constexpr std::size_t covered = offsetof(FileHeader, checksum);
  auto bytes = std::array<char, covered>{};
  std::memcpy(bytes.data(), &header, covered);
  return Crc32(bytes.data(), covered);
}

}  // namespace tinydb
