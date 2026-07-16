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

// Semantic checks that cannot be expressed by field widths alone. Encoding
// reports these as InvalidArgument because the caller supplied impossible
// state; decoding reports the same findings as Corruption because the state
// came from persistent bytes.
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
  // Zero is the null page reference. Every real metadata root must name a
  // page that has already been allocated below the high-water frontier.
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

// The checksum protects the complete 4 KiB page, including reserved zeros.
// Treating the checksum field itself as zero makes encoding and verification
// use one deterministic calculation without special CRC concatenation logic.
auto ChecksumPage(std::span<const std::byte> input) -> std::uint32_t {
  auto page = SuperblockPage{};
  std::ranges::copy(input, page.begin());
  std::ranges::fill(page.begin() + static_cast<std::ptrdiff_t>(superblock_offset::CHECKSUM),
                    page.begin() + static_cast<std::ptrdiff_t>(superblock_offset::CHECKSUM + sizeof(std::uint32_t)),
                    std::byte{0});
  return Crc32(page);
}

auto DecodeError(StatusCode code, std::string message) -> Status {
  // Selection collapses two detailed decode failures into one public result,
  // while preserving the important "recognized but damaged" distinction.
  if (code == StatusCode::UnsupportedFormat) {
    return Status::UnsupportedFormat(std::move(message));
  }
  return Status::Corruption(std::move(message));
}

}  // namespace

auto EncodeSuperblock(const Superblock &superblock) -> Result<SuperblockPage> {
  // An encoder must never mint bytes that this binary has already declared it
  // cannot interpret. Optional bits are different: preserving an unknown
  // optional bit cannot change the meaning required for safe reading.
  if ((superblock.required_features & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("cannot encode unsupported required superblock features"));
  }
  if (const auto invalid = InvalidSuperblock(superblock)) {
    return std::unexpected(Status::InvalidArgument(*invalid));
  }

  // Value-initialization zeroes every reserved byte. Besides preventing stale
  // memory disclosure, this gives future decoders a canonical extension area.
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
    // Offsets are compile-time constants, so reaching this branch indicates a
    // codec maintenance bug rather than malformed user input.
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
    // Check magic before CRC so an old or foreign file is reported as an
    // unsupported format, not misleadingly as a damaged current superblock.
    return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB superblock magic"));
  }

  const auto stored_checksum = GetLittleEndian<std::uint32_t>(page, superblock_offset::CHECKSUM);
  if (!stored_checksum || *stored_checksum != ChecksumPage(page)) {
    return std::unexpected(Status::Corruption("superblock checksum mismatch"));
  }

  // Do not interpret state-bearing page IDs until framing, checksum, version,
  // page size, and required feature semantics have all been accepted.
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

  // Current writers canonicalize unused bytes to zero. Rejecting nonzero
  // reserved bytes prevents silent acceptance of a format extension whose
  // semantics this reader does not understand.
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
  // Decode independently: damage to one slot must not prevent the other slot
  // from opening the database.
  auto first = DecodeSuperblock(page_a);
  auto second = DecodeSuperblock(page_b);

  if (first && second) {
    // Equal generations are expected after creation and checkpoint mirroring.
    // They must describe identical logical state; differing state would make
    // the chosen root/free-list nondeterministic across readers.
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
  // If either slot looked like this format but failed integrity validation,
  // report corruption. UnsupportedFormat is reserved for the case where both
  // slots are simply not a format this binary recognizes.
  return std::unexpected(DecodeError(code, "neither superblock is valid"));
}

}  // namespace tinydb::storage
