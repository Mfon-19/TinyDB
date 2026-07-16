#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::storage {

using SuperblockPage = std::array<std::byte, PAGE_SIZE>;

inline constexpr auto SUPERBLOCK_MAGIC = std::array{
    std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59},
    std::byte{0x44}, std::byte{0x42}, std::byte{0x30}, std::byte{0x34},
};  // "TINYDB04"
inline constexpr std::uint16_t FORMAT_MAJOR = 1;
inline constexpr std::uint16_t FORMAT_MINOR = 0;
inline constexpr std::uint64_t SUPPORTED_REQUIRED_FEATURES = 0;
inline constexpr page_id_t FIRST_FORMAT_DATA_PAGE_ID = FIRST_DATA_PAGE_ID;

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
  DatabaseUuid database_uuid{};
  std::uint64_t generation{1};
  std::uint64_t checkpoint_lsn{0};
  std::uint64_t transaction_id{0};
  page_id_t root_page_id{0};
  page_id_t allocator_root_page_id{0};
  page_id_t high_water_page_id{FIRST_FORMAT_DATA_PAGE_ID};
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

auto EncodeSuperblock(const Superblock &superblock) -> Result<SuperblockPage>;
auto DecodeSuperblock(std::span<const std::byte> page) -> Result<Superblock>;
auto SelectSuperblock(std::span<const std::byte> page_a,
                      std::span<const std::byte> page_b) -> Result<SelectedSuperblock>;

}  // namespace tinydb::storage
