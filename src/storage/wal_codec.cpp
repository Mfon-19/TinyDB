#include "tinydb/storage/wal_codec.h"
#include "tinydb/storage/crc32.h"
#include "tinydb/storage/encoding.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace tinydb::storage {

namespace {

constexpr std::array<char, 4> MAGIC{'T', 'D', 'W', '1'};
constexpr std::size_t HEADER_SIZE = 8;
constexpr std::size_t FRAME_SIZE = sizeof(PageId) + PAGE_SIZE;
constexpr std::size_t CRC_SIZE = sizeof(std::uint32_t);
constexpr std::size_t RECORD_OVERHEAD = HEADER_SIZE + CRC_SIZE;

bool ValidPageId(PageId page_id) {
  return page_id != 0 && page_id != INVALID_PAGE_ID;
}

} // namespace

auto WalRecordSize(std::size_t frame_count) -> Result<std::size_t> {
  if (frame_count == 0 ||
      frame_count > std::numeric_limits<std::uint32_t>::max()) {
    return Err(Status::InvalidArgument("invalid WAL frame count"));
  }
  if (frame_count >
      (std::numeric_limits<std::size_t>::max() - RECORD_OVERHEAD) /
          FRAME_SIZE) {
    return Err(Status::InvalidArgument("WAL record size overflow"));
  }
  return RECORD_OVERHEAD + FRAME_SIZE * frame_count;
}

auto EncodeWalRecord(const WalPages &pages) -> Result<std::vector<char>> {
  auto size = WalRecordSize(pages.size());
  if (!size) {
    return Err(std::move(size.error()));
  }
  for (const auto &[page_id, page] : pages) {
    if (!ValidPageId(page_id)) {
      return Err(Status::InvalidArgument("invalid WAL page ID"));
    }
  }
  std::vector<char> bytes;
  if (*size > bytes.max_size()) {
    return Err(Status::ResourceExhausted("WAL record is too large"));
  }
  bytes.resize(*size);
  std::ranges::copy(MAGIC, bytes.begin());
  little_endian::PutU32(bytes, 4, static_cast<std::uint32_t>(pages.size()));
  std::size_t offset = HEADER_SIZE;
  for (const auto &[page_id, page] : pages) {
    little_endian::PutU32(bytes, offset, page_id);
    std::ranges::copy(page, bytes.begin() + offset + sizeof(PageId));
    offset += FRAME_SIZE;
  }
  little_endian::PutU32(bytes, offset,
                        Crc32(std::span<const char>{bytes}.first(offset)));
  return bytes;
}

auto DecodeWal(std::span<const char> bytes) -> Result<WalPages> {
  WalPages pages;
  while (bytes.size() >= HEADER_SIZE) {
    if (!std::ranges::equal(MAGIC, bytes.first(MAGIC.size()))) {
      return Err(Status::Corruption("invalid WAL magic"));
    }
    const auto frame_count = little_endian::GetU32(bytes, 4);
    auto size = WalRecordSize(frame_count);
    if (!size) {
      return Err(Status::Corruption(std::string{size.error().Message()}));
    }
    if (*size > bytes.size()) {
      break;
    }

    const auto record = bytes.first(*size);
    const std::size_t crc_offset = record.size() - CRC_SIZE;
    for (std::size_t offset = HEADER_SIZE; offset < crc_offset;
         offset += FRAME_SIZE) {
      if (!ValidPageId(little_endian::GetU32(record, offset))) {
        return Err(Status::Corruption("invalid WAL page ID"));
      }
    }
    if (little_endian::GetU32(record, crc_offset) !=
        Crc32(record.first(crc_offset))) {
      if (record.size() == bytes.size()) {
        break;
      }
      return Err(Status::Corruption("WAL checksum mismatch before end of log"));
    }

    for (std::size_t offset = HEADER_SIZE; offset < crc_offset;
         offset += FRAME_SIZE) {
      const auto page_id = little_endian::GetU32(record, offset);
      PageBytes page;
      std::ranges::copy(record.subspan(offset + sizeof(PageId), PAGE_SIZE),
                        page.begin());
      pages.insert_or_assign(page_id, std::move(page));
    }
    bytes = bytes.subspan(record.size());
  }
  return pages;
}

} // namespace tinydb::storage
