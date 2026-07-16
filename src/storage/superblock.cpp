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

auto ChecksumPage(std::span<const std::byte> input) -> std::uint32_t {
  auto page = SuperblockPage{};
  std::ranges::copy(input, page.begin());
  std::ranges::fill(page.begin() + static_cast<std::ptrdiff_t>(superblock_offset::CHECKSUM),
                    page.begin() + static_cast<std::ptrdiff_t>(superblock_offset::CHECKSUM + sizeof(std::uint32_t)),
                    std::byte{0});
  return Crc32(page);
}

auto DecodeError(StatusCode code, std::string message) -> Status {
  if (code == StatusCode::UnsupportedFormat) {
    return Status::UnsupportedFormat(std::move(message));
  }
  return Status::Corruption(std::move(message));
}

}  // namespace

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
      PutLittleEndian(bytes, superblock_offset::TRANSACTION_ID, superblock.transaction_id) &&
      PutLittleEndian(bytes, superblock_offset::ROOT_PAGE_ID, superblock.root_page_id) &&
      PutLittleEndian(bytes, superblock_offset::ALLOCATOR_ROOT_PAGE_ID, superblock.allocator_root_page_id) &&
      PutLittleEndian(bytes, superblock_offset::HIGH_WATER_PAGE_ID, superblock.high_water_page_id);
  if (!encoded) {
    return std::unexpected(Status::Corruption("internal superblock layout exceeds one page"));
  }
  if (!PutLittleEndian(bytes, superblock_offset::CHECKSUM, ChecksumPage(page))) {
    return std::unexpected(Status::Corruption("superblock checksum field exceeds one page"));
  }
  return page;
}

auto DecodeSuperblock(std::span<const std::byte> page) -> Result<Superblock> {
  if (page.size() != PAGE_SIZE) {
    return std::unexpected(Status::Corruption("superblock is not exactly one page"));
  }
  if (!std::ranges::equal(SUPERBLOCK_MAGIC, page.subspan(superblock_offset::MAGIC, SUPERBLOCK_MAGIC.size()))) {
    return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB superblock magic"));
  }

  const auto stored_checksum = GetLittleEndian<std::uint32_t>(page, superblock_offset::CHECKSUM);
  if (!stored_checksum || *stored_checksum != ChecksumPage(page)) {
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
  const auto transaction_id = GetLittleEndian<std::uint64_t>(page, superblock_offset::TRANSACTION_ID);
  const auto root_page_id = GetLittleEndian<page_id_t>(page, superblock_offset::ROOT_PAGE_ID);
  const auto allocator_root_page_id = GetLittleEndian<page_id_t>(page, superblock_offset::ALLOCATOR_ROOT_PAGE_ID);
  const auto high_water_page_id = GetLittleEndian<page_id_t>(page, superblock_offset::HIGH_WATER_PAGE_ID);
  if (!generation || !checkpoint_lsn || !transaction_id || !root_page_id || !allocator_root_page_id ||
      !high_water_page_id) {
    return std::unexpected(Status::Corruption("truncated superblock state fields"));
  }
  superblock.generation = *generation;
  superblock.checkpoint_lsn = *checkpoint_lsn;
  superblock.transaction_id = *transaction_id;
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

auto SelectSuperblock(std::span<const std::byte> page_a,
                      std::span<const std::byte> page_b) -> Result<SelectedSuperblock> {
  auto first = DecodeSuperblock(page_a);
  auto second = DecodeSuperblock(page_b);

  if (first && second) {
    if (first->generation == second->generation && *first != *second) {
      return std::unexpected(Status::Corruption("superblocks have the same generation but different state"));
    }
    if (second->generation > first->generation) {
      return SelectedSuperblock{.value = *second, .slot = SuperblockSlot::B};
    }
    return SelectedSuperblock{.value = *first, .slot = SuperblockSlot::A};
  }
  if (first) {
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
