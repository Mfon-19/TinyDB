#include "tinydb/storage/superblock_codec.h"
#include "tinydb/storage/crc32.h"
#include "tinydb/storage/encoding.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::storage {

namespace {

inline constexpr std::array<char, 8> SUPERBLOCK_MAGIC = {'T', 'I', 'N', 'Y',
                                                         'D', 'B', '0', '1'};
inline constexpr std::uint16_t FORMAT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t PAGE_SIZE_OFFSET = 10;
inline constexpr std::size_t ROOT_PAGE_ID_OFFSET = 12;
inline constexpr std::size_t CHECKSUM_OFFSET = 16;

} // namespace

auto EncodeSuperblock(const Superblock &superblock) -> Result<PageBytes> {
  if (superblock.root_page_id == 0 ||
      superblock.root_page_id == INVALID_PAGE_ID) {
    return Err(Status::InvalidArgument("invalid root page ID"));
  }

  PageBytes page{};
  std::ranges::copy(SUPERBLOCK_MAGIC, page.begin() + MAGIC_OFFSET);
  little_endian::PutU16(page, VERSION_OFFSET, FORMAT_VERSION);
  little_endian::PutU16(page, PAGE_SIZE_OFFSET,
                        static_cast<std::uint16_t>(PAGE_SIZE));
  little_endian::PutU32(page, ROOT_PAGE_ID_OFFSET, superblock.root_page_id);
  little_endian::PutU32(
      page, CHECKSUM_OFFSET,
      Crc32WithZeroedU32(std::span<const char>{page}, CHECKSUM_OFFSET));
  return page;
}

auto DecodeSuperblock(const PageBytes &page) -> Result<Superblock> {
  const std::span<const char> bytes{page};
  if (!std::ranges::equal(
          SUPERBLOCK_MAGIC,
          bytes.subspan(MAGIC_OFFSET, SUPERBLOCK_MAGIC.size()))) {
    return Err(Status::Corruption("invalid superblock magic"));
  }

  if (little_endian::GetU16(bytes, VERSION_OFFSET) != FORMAT_VERSION) {
    return Err(Status::Corruption("unsupported superblock version"));
  }

  if (little_endian::GetU16(bytes, PAGE_SIZE_OFFSET) != PAGE_SIZE) {
    return Err(Status::Corruption("invalid superblock page size"));
  }

  const auto stored_checksum = little_endian::GetU32(bytes, CHECKSUM_OFFSET);
  if (stored_checksum != Crc32WithZeroedU32(bytes, CHECKSUM_OFFSET)) {
    return Err(Status::Corruption("superblock checksum mismatch"));
  }

  const auto root_page_id = little_endian::GetU32(bytes, ROOT_PAGE_ID_OFFSET);
  if (root_page_id == 0 || root_page_id == INVALID_PAGE_ID) {
    return Err(Status::Corruption("invalid superblock root page ID"));
  }

  return Superblock{root_page_id};
}

} // namespace tinydb::storage
