#pragma once

#include <tinydb/status.h>
#include "storage/database_uuid.h"
#include "storage/page.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::storage {

/*
** SUPERBLOCK FORMAT AND SELECTION
**
** Pages 0 and 1 each hold one complete database-state root. Updates normally
** target the inactive slot with a monotonically increasing generation. A torn
** metadata write can therefore invalidate at most the copy being replaced.
** Opening decodes the slots independently and chooses the valid copy with the
** greatest generation.
**
** If both copies have the same generation they must describe identical state;
** otherwise there is no principled winner and opening reports corruption. One
** valid copy is sufficient. Unknown required features reject the format, while
** optional feature bits may be retained and ignored.
**
** All fields use explicit little-endian offsets. Reserved bytes are zero and
** participate in the page checksum. No native C++ object representation is
** ever copied into the database file.
*/
using SuperblockPage = std::array<std::byte, PAGE_SIZE>;

inline constexpr auto SUPERBLOCK_MAGIC = std::array{
    std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59},
    std::byte{0x44}, std::byte{0x42}, std::byte{0x30}, std::byte{0x34},
};  // "TINYDB04"
inline constexpr std::uint16_t FORMAT_MAJOR = 1;
inline constexpr std::uint16_t FORMAT_MINOR = 0;
inline constexpr std::uint64_t SUPPORTED_REQUIRED_FEATURES = 0;
inline constexpr page_id_t FIRST_FORMAT_DATA_PAGE_ID = FIRST_DATA_PAGE_ID;

/*
** Stable byte offsets in the on-disk superblock:
**
**   0   magic[8]                 48  generation u64
**   8   format major/minor       56  checkpoint LSN u64
**   12  page size u32            64  transaction ID u64
**   16  required features u64    72  B+ tree root page ID u64
**   24  optional features u64    80  allocator root page ID u64
**   32  database UUID[16]        88  high-water page ID u64
**   96  CRC-32 u32              100..4095 reserved zero bytes
*/
namespace superblock_offset {
inline constexpr std::size_t MAGIC = 0;
inline constexpr std::size_t FORMAT_MAJOR = 8;
inline constexpr std::size_t FORMAT_MINOR = 10;
inline constexpr std::size_t PAGE_SIZE = 12;
inline constexpr std::size_t REQUIRED_FEATURES = 16;
inline constexpr std::size_t OPTIONAL_FEATURES = 24;
inline constexpr std::size_t DATABASE_UUID = 32;
inline constexpr std::size_t GENERATION = 48;
inline constexpr std::size_t CHECKPOINT_LSN = 56;
inline constexpr std::size_t TRANSACTION_ID = 64;
inline constexpr std::size_t ROOT_PAGE_ID = 72;
inline constexpr std::size_t ALLOCATOR_ROOT_PAGE_ID = 80;
inline constexpr std::size_t HIGH_WATER_PAGE_ID = 88;
inline constexpr std::size_t CHECKSUM = 96;
inline constexpr std::size_t ENCODED_BYTES = 100;
}  // namespace superblock_offset

struct Superblock {
  // UUID prevents a WAL copied from another database from being replayed.
  DatabaseUuid database_uuid{};

  // Generation chooses the newest valid slot. Zero is never valid.
  std::uint64_t generation{1};

  // Recovery can ignore WAL history at or before this durable frontier.
  std::uint64_t checkpoint_lsn{0};

  // Identity of the last transaction represented by this state.
  std::uint64_t transaction_id{0};

  // Zero means the corresponding persistent tree/index is currently absent.
  page_id_t root_page_id{0};
  page_id_t allocator_root_page_id{0};

  // First page ID not yet allocated. Every nonzero reference must be below
  // this value and at or above FIRST_FORMAT_DATA_PAGE_ID.
  page_id_t high_water_page_id{FIRST_FORMAT_DATA_PAGE_ID};

  // Unknown required bits make the format unsafe to interpret; unknown
  // optional bits may be preserved and ignored.
  std::uint64_t required_features{0};
  std::uint64_t optional_features{0};

  auto operator==(const Superblock &) const -> bool = default;
};

enum class SuperblockSlot {
  A,
  B,
};

struct SelectedSuperblock {
  Superblock value;
  SuperblockSlot slot;
};

// Encode always zero-fills the complete page before writing fields. Reserved
// bytes therefore participate in the checksum and remain available for future
// compatible extensions without exposing stale memory to disk.
auto EncodeSuperblock(const Superblock &superblock) -> Result<SuperblockPage>;

// Decode distinguishes an unknown format (UnsupportedFormat) from damage to a
// recognized format (Corruption). Callers use that distinction to reject old
// files without accidentally treating them as repairable TinyDB databases.
auto DecodeSuperblock(std::span<const std::byte> page) -> Result<Superblock>;

// Selects the highest valid generation. A single valid copy is sufficient;
// equal generations must be byte-for-byte equivalent in logical state because
// there is otherwise no principled way to decide which one won.
auto SelectSuperblock(std::span<const std::byte> page_a,
                      std::span<const std::byte> page_b) -> Result<SelectedSuperblock>;

}  // namespace tinydb::storage
