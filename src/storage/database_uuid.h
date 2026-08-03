#pragma once

#include <array>
#include <cstddef>

namespace tinydb {

// Identifies one database. TinyDB stores the same 16-byte value in both
// superblocks and the WAL header. This prevents the use of a WAL from another
// database.
using DatabaseUuid = std::array<std::byte, 16>;

}  // namespace tinydb
