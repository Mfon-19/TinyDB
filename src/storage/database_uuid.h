#pragma once

#include <array>
#include <cstddef>

namespace tinydb {

// Persistent identity shared by the database superblocks and every WAL file
// that belongs to them. It is deliberately an opaque 128-bit value: storage
// code compares and copies the bytes, but never depends on a textual UUID
// spelling or a platform-specific UUID library representation.
//
// The all-zero value is reserved as "not initialized" and is rejected by the
// persistent codecs. That lets a torn, zero-filled creation be distinguished
// from a valid database without relying on any in-memory state.
using DatabaseUuid = std::array<std::byte, 16>;

}  // namespace tinydb
