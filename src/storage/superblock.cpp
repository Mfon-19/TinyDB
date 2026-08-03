#include "storage/superblock.h"

#include "storage/encoding.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace tinydb::storage {
namespace {

// Finds the first invalid value or relationship in the superblock metadata.
// It returns a description of the error, or no value when the metadata is
// valid. The encoder reports this error as InvalidArgument. The decoder reports
// it as Corruption.
auto InvalidSuperblock(const Superblock &superblock) -> std::optional<std::string> {
  if (std::ranges::all_of(superblock.database_uuid, [](std::byte byte) { return byte == std::byte{0}; })) {
    return "database UUID is zero";
  }
  if (superblock.generation == 0) {
    return "generation is zero";
  }
  if (superblock.high_water_page_id < FIRST_FORMAT_DATA_PAGE_ID) {
    return "high-water page ID overlaps the superblocks";
  }

  const auto valid_reference = [&](page_id_t page_id) {
    return page_id == 0 || (page_id >= FIRST_FORMAT_DATA_PAGE_ID && page_id < superblock.high_water_page_id);
  };
  if (!valid_reference(superblock.root_page_id)) {
    return "root page ID is outside the allocation frontier";
  }
  if (!valid_reference(superblock.allocator_root_page_id)) {
    return "allocator root page ID is outside the allocation frontier";
  }
  return std::nullopt;
}

// Creates the combined error used when neither superblock slot is usable. It
// preserves UnsupportedFormat and maps every other status code to Corruption.
auto DecodeError(StatusCode code, std::string message) -> Status {
  if (code == StatusCode::UnsupportedFormat) {
    return Status::UnsupportedFormat(std::move(message));
  }
  return Status::Corruption(std::move(message));
}

}  // namespace

// Encodes logical Superblock metadata into one page after it applies the format
// rules. It uses fixed little-endian offsets, preserves optional feature bits,
// fills reserved bytes with zero, and adds a CRC-32 checksum. It reports unknown
// required features as UnsupportedFormat. It reports other invalid metadata as
// InvalidArgument.
auto EncodeSuperblock(const Superblock &superblock) -> Result<SuperblockPage> {
  if ((superblock.required_features & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("cannot encode unsupported required superblock features"));
  }
  if (const auto invalid = InvalidSuperblock(superblock)) {
    return std::unexpected(Status::InvalidArgument(*invalid));
  }

  auto page = SuperblockPage{};
  auto bytes = std::span<std::byte>{page};
  const auto encoded =
      PutBytes(bytes, superblock_offset::MAGIC, SUPERBLOCK_MAGIC) &&
      PutLittleEndian(bytes, superblock_offset::FORMAT_MAJOR, FORMAT_MAJOR) &&
      PutLittleEndian(bytes, superblock_offset::FORMAT_MINOR, FORMAT_MINOR) &&
      PutLittleEndian(bytes, superblock_offset::PAGE_SIZE, static_cast<std::uint32_t>(PAGE_SIZE)) &&
      PutLittleEndian(bytes, superblock_offset::REQUIRED_FEATURES, superblock.required_features) &&
      PutLittleEndian(bytes, superblock_offset::OPTIONAL_FEATURES, superblock.optional_features) &&
      PutBytes(bytes, superblock_offset::DATABASE_UUID, superblock.database_uuid) &&
      PutLittleEndian(bytes, superblock_offset::GENERATION, superblock.generation) &&
      PutLittleEndian(bytes, superblock_offset::CHECKPOINT_LSN, superblock.checkpoint_lsn) &&
      PutLittleEndian(bytes, superblock_offset::ROOT_PAGE_ID, superblock.root_page_id) &&
      PutLittleEndian(bytes, superblock_offset::ALLOCATOR_ROOT_PAGE_ID, superblock.allocator_root_page_id) &&
      PutLittleEndian(bytes, superblock_offset::HIGH_WATER_PAGE_ID, superblock.high_water_page_id);
  if (!encoded) {
    return std::unexpected(Status::Corruption("internal superblock layout exceeds one page"));
  }
  if (!PutLittleEndian(bytes, superblock_offset::CHECKSUM, Crc32WithZeroedU32(bytes, superblock_offset::CHECKSUM))) {
    return std::unexpected(Status::Corruption("superblock checksum field exceeds one page"));
  }
  return page;
}

// Decodes one complete on-disk page into logical Superblock metadata. It
// examines the page length, magic, checksum, format fields, reserved bytes, and
// relationships between values. It reports unknown or incompatible formats as
// UnsupportedFormat. It reports malformed or inconsistent pages as Corruption.
auto DecodeSuperblock(std::span<const std::byte> page) -> Result<Superblock> {
  if (page.size() != PAGE_SIZE) {
    return std::unexpected(Status::Corruption("superblock is not exactly one page"));
  }
  if (!std::ranges::equal(SUPERBLOCK_MAGIC, page.subspan(superblock_offset::MAGIC, SUPERBLOCK_MAGIC.size()))) {
    return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB superblock magic"));
  }

  const auto stored_checksum = GetLittleEndian<std::uint32_t>(page, superblock_offset::CHECKSUM);
  if (!stored_checksum || *stored_checksum != Crc32WithZeroedU32(page, superblock_offset::CHECKSUM)) {
    return std::unexpected(Status::Corruption("superblock checksum mismatch"));
  }

  const auto major = GetLittleEndian<std::uint16_t>(page, superblock_offset::FORMAT_MAJOR);
  const auto minor = GetLittleEndian<std::uint16_t>(page, superblock_offset::FORMAT_MINOR);
  const auto page_size = GetLittleEndian<std::uint32_t>(page, superblock_offset::PAGE_SIZE);
  const auto required = GetLittleEndian<std::uint64_t>(page, superblock_offset::REQUIRED_FEATURES);
  const auto optional = GetLittleEndian<std::uint64_t>(page, superblock_offset::OPTIONAL_FEATURES);
  if (!major || !minor || !page_size || !required || !optional) {
    return std::unexpected(Status::Corruption("truncated superblock format fields"));
  }
  if (*major != FORMAT_MAJOR || *minor > FORMAT_MINOR || *page_size != PAGE_SIZE ||
      (*required & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("unsupported TinyDB superblock version or features"));
  }

  auto superblock = Superblock{};
  if (!GetBytes(page, superblock_offset::DATABASE_UUID, superblock.database_uuid)) {
    return std::unexpected(Status::Corruption("truncated database UUID"));
  }
  const auto generation = GetLittleEndian<std::uint64_t>(page, superblock_offset::GENERATION);
  const auto checkpoint_lsn = GetLittleEndian<std::uint64_t>(page, superblock_offset::CHECKPOINT_LSN);
  const auto reserved = GetLittleEndian<std::uint64_t>(page, superblock_offset::RESERVED);
  const auto root_page_id = GetLittleEndian<page_id_t>(page, superblock_offset::ROOT_PAGE_ID);
  const auto allocator_root_page_id = GetLittleEndian<page_id_t>(page, superblock_offset::ALLOCATOR_ROOT_PAGE_ID);
  const auto high_water_page_id = GetLittleEndian<page_id_t>(page, superblock_offset::HIGH_WATER_PAGE_ID);
  if (!generation || !checkpoint_lsn || !reserved || !root_page_id || !allocator_root_page_id || !high_water_page_id ||
      *reserved != 0) {
    return std::unexpected(Status::Corruption("truncated superblock state fields"));
  }
  superblock.generation = *generation;
  superblock.checkpoint_lsn = *checkpoint_lsn;
  superblock.root_page_id = *root_page_id;
  superblock.allocator_root_page_id = *allocator_root_page_id;
  superblock.high_water_page_id = *high_water_page_id;
  superblock.required_features = *required;
  superblock.optional_features = *optional;

  if (const auto nonzero = std::ranges::find_if(page.subspan(superblock_offset::ENCODED_BYTES),
                                                [](std::byte byte) { return byte != std::byte{0}; });
      nonzero != page.end()) {
    return std::unexpected(Status::Corruption("nonzero reserved superblock bytes"));
  }
  if (const auto invalid = InvalidSuperblock(superblock)) {
    return std::unexpected(Status::Corruption(*invalid));
  }
  return superblock;
}

// Decodes both slots independently and returns the selected metadata with its
// slot. The selected slot tells the next checkpoint which page it must
// preserve. If both slots are valid, the larger generation wins. One valid slot
// is sufficient. Equal valid generations must contain identical metadata.
// If neither slot is usable, Corruption takes priority over UnsupportedFormat.
auto SelectSuperblock(std::span<const std::byte> page_a,
                      std::span<const std::byte> page_b) -> Result<SelectedSuperblock> {
  auto first = DecodeSuperblock(page_a);
  auto second = DecodeSuperblock(page_b);

  if (first && second) {
    // Equal generations provide no ordering information. Different metadata
    // would make either choice unsafe.
    if (first->generation == second->generation && *first != *second) {
      return std::unexpected(Status::Corruption("superblocks have the same generation but different state"));
    }
    if (second->generation > first->generation) {
      return SelectedSuperblock{.value = *second, .slot = SuperblockSlot::B};
    }
    return SelectedSuperblock{.value = *first, .slot = SuperblockSlot::A};
  }
  if (first) {
    // The next checkpoint restores the second copy in the other slot.
    return SelectedSuperblock{.value = *first, .slot = SuperblockSlot::A};
  }
  if (second) {
    return SelectedSuperblock{.value = *second, .slot = SuperblockSlot::B};
  }

  const auto code = first.error().Code() == StatusCode::Corruption || second.error().Code() == StatusCode::Corruption
                        ? StatusCode::Corruption
                        : StatusCode::UnsupportedFormat;
  return std::unexpected(DecodeError(code, "neither superblock is valid"));
}

}  // namespace tinydb::storage
