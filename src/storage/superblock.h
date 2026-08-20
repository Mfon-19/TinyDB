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
** Pages 0 and 1 of the database are superblock slots A and B, each containing
** a complete description of one durable checkpoint; the slot with the largest
** generation is the selected slot.
**
** A checkpoint is published in the following order:
**
**   1. Synchronize all changed data pages.
**   2. Copy the selected metadata and increase its generation.
**   3. Write the new metadata to the slot that is not selected.
**   4. Synchronize the database file.
**   5. Select the new slot in memory.
**
** The old slot is not modified during this sequence, so if a crash leaves the
** new slot incomplete, the old slot and the WAL still describe a recoverable
** state; in-memory metadata changes only after the new slot is durable.
**
** Opening decodes the slots independently and can proceed with one valid slot.
** If both are valid, the larger generation wins; equal generations must
** contain identical metadata because neither slot can be identified as newer.
**
** SuperblockPage is the exact PAGE_SIZE byte image stored in one slot, with
** fields written at explicit little-endian offsets instead of copied from the
** C++ Superblock object.  Reserved bytes are zero and covered by the checksum;
** unknown required features are rejected, while unknown optional features are
** preserved but otherwise ignored.
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
** The on-disk layout of a superblock is as follows:
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
  DatabaseUuid database_uuid{};

  // Zero is not a valid generation.
  std::uint64_t generation{1};

  // WAL records at or before this LSN are already present in the database.
  std::uint64_t checkpoint_lsn{0};

  // Zero means that the corresponding persistent root is absent.
  page_id_t root_page_id{0};
  page_id_t allocator_root_page_id{0};

  // This count includes the two superblocks and all allocated or reusable
  // data-page slots; a valid data-page reference is less than this value.
  page_id_t logical_page_count{FIRST_FORMAT_DATA_PAGE_ID};

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

/*
** Encode superblock into a new page, first filling the page with zero so that
** reserved bytes participate in the checksum and cannot expose stale memory.
** Return UnsupportedFormat for unknown required features or InvalidArgument
** for invalid metadata.
*/
auto EncodeSuperblock(const Superblock &superblock) -> Result<SuperblockPage>;

/*
** Decode one complete superblock page, returning UnsupportedFormat if the
** magic, version, page size, or required features are not supported, or
** Corruption if a recognized page has an invalid checksum, reserved field, or
** metadata relationship.
*/
auto DecodeSuperblock(std::span<const std::byte> page) -> Result<Superblock>;

/*
** Decode both superblock slots and return the valid slot with the largest
** generation.  A single valid slot is sufficient, but if two valid slots have
** the same generation, their metadata must be identical.
*/
auto SelectSuperblock(std::span<const std::byte> page_a,
                      std::span<const std::byte> page_b) -> Result<SelectedSuperblock>;

}  // namespace tinydb::storage
