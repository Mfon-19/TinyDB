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
** SUPERBLOCK PAGE AND STORAGE PROTOCOL
**
** SuperblockPage holds the exact PAGE_SIZE bytes stored in one superblock
** slot. It separates the on-disk byte format from the decoded Superblock
** values used by the program. Its fixed size makes each superblock read and
** write cover exactly one database page.
**
** Database pages 0 and 1 are slots A and B. Each slot contains a complete copy
** of the metadata for one durable checkpoint. Two slots keep the previous copy
** safe while TinyDB writes the next copy.
**
** The checkpoint protocol is:
**
**   1. The checkpoint manager synchronizes all changed data pages.
**   2. TinyDB copies the current metadata, increases its generation, and
**      records the new checkpoint state.
**   3. TinyDB encodes the metadata in a SuperblockPage.
**   4. TinyDB writes the page to the slot that is not currently selected.
**   5. TinyDB synchronizes the database file.
**   6. TinyDB selects the new slot in memory.
**
** A crash during the write can leave the new slot incomplete. The previous
** slot remains valid because this protocol does not modify it.
**
** Opening decodes both slots independently. It selects the valid slot with the
** larger generation. One valid slot is sufficient. If both valid slots have
** the same generation, their decoded metadata must match. Otherwise, TinyDB
** reports corruption because it cannot identify one state as newer.
**
** The encoder writes each field at an explicit little-endian offset. It fills
** reserved bytes with zero and includes them in the checksum. It never copies
** the compiler-dependent memory layout of Superblock into the database file.
** TinyDB rejects unknown required features. It preserves and ignores unknown
** optional features.
*/
using SuperblockPage = std::array<std::byte, PAGE_SIZE>;

inline constexpr auto SUPERBLOCK_MAGIC = std::array{
    std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59},
    std::byte{0x44}, std::byte{0x42}, std::byte{0x30}, std::byte{0x35},
};  // "TINYDB05"
inline constexpr std::uint16_t FORMAT_MAJOR = 1;
inline constexpr std::uint16_t FORMAT_MINOR = 0;
inline constexpr std::uint64_t SUPPORTED_REQUIRED_FEATURES = 0;
inline constexpr page_id_t FIRST_FORMAT_DATA_PAGE_ID = FIRST_DATA_PAGE_ID;

/*
** Stable byte offsets in the on-disk superblock:
**
**   0   magic[8]                 48  generation u64
**   8   format major/minor       56  checkpoint LSN u64
**   12  page size u32            64  reserved u64
**   16  required features u64    72  B+ tree root page ID u64
**   24  optional features u64    80  allocator root page ID u64
**   32  database UUID[16]        88  logical page count u64
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
inline constexpr std::size_t RESERVED = 64;
inline constexpr std::size_t ROOT_PAGE_ID = 72;
inline constexpr std::size_t ALLOCATOR_ROOT_PAGE_ID = 80;
inline constexpr std::size_t LOGICAL_PAGE_COUNT = 88;
inline constexpr std::size_t CHECKSUM = 96;
inline constexpr std::size_t ENCODED_BYTES = 100;
}  // namespace superblock_offset

struct Superblock {
  // UUID prevents a WAL copied from another database from being replayed.
  DatabaseUuid database_uuid{};

  // Generation chooses the newest valid slot. Zero is never valid.
  std::uint64_t generation{1};

  // Recovery can ignore WAL records at or before this LSN.
  std::uint64_t checkpoint_lsn{0};

  // Zero means the corresponding persistent tree/index is currently absent.
  page_id_t root_page_id{0};
  page_id_t allocator_root_page_id{0};

  // Number of logical page slots, including both superblocks and reusable
  // data-page slots. Valid data-page references are less than this count.
  page_id_t logical_page_count{FIRST_FORMAT_DATA_PAGE_ID};

  // Unknown required bits make the format unsafe to interpret. TinyDB
  // preserves and ignores unknown optional bits.
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
