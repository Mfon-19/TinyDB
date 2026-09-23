#include "tinydb/storage/wal_codec.h"
#include "tinydb/storage/crc32.h"
#include "tinydb/storage/encoding.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace tinydb::storage {

namespace {

constexpr std::array<char, 4> MAGIC{'T', 'D', 'W', '3'};
constexpr std::size_t SALT_OFFSET = 4;
constexpr std::size_t HEADER_CRC_OFFSET = 8;
constexpr std::size_t HEADER_SIZE = 8;
constexpr std::size_t FRAME_SIZE = sizeof(PageId) + PAGE_SIZE;
constexpr std::size_t CRC_SIZE = sizeof(std::uint32_t);
constexpr std::size_t RECORD_OVERHEAD = HEADER_SIZE + CRC_SIZE;

void AddFrame(Crc32Accumulator &crc, PageId page_id,
              std::uint32_t checksum) noexcept {
  std::array<char, 2 * sizeof(std::uint32_t)> frame;
  little_endian::PutU32(frame, 0, page_id);
  little_endian::PutU32(frame, sizeof(std::uint32_t), checksum);
  crc.Update(frame);
}

} // namespace

auto EncodeWalHeader(std::uint32_t salt) -> std::array<char, WAL_HEADER_SIZE> {
  std::array<char, WAL_HEADER_SIZE> header{};
  std::ranges::copy(MAGIC, header.begin());
  little_endian::PutU32(header, SALT_OFFSET, salt);
  little_endian::PutU32(
      header, HEADER_CRC_OFFSET,
      Crc32(std::span<const char>{header}.first(HEADER_CRC_OFFSET)));
  return header;
}

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

auto EncodeWalRecord(const PageMap &pages,
                     std::uint32_t salt) -> Result<std::vector<char>> {
  auto size = WalRecordSize(pages.size());
  if (!size) {
    return Err(std::move(size.error()));
  }

  std::vector<char> bytes;
  if (*size > bytes.max_size()) {
    return Err(Status::ResourceExhausted("WAL record is too large"));
  }

  bytes.resize(*size);
  little_endian::PutU32(bytes, 0, salt);
  little_endian::PutU32(bytes, 4, static_cast<std::uint32_t>(pages.size()));

  Crc32Accumulator crc;
  crc.Update(std::span<const char>{bytes}.first(HEADER_SIZE));
  std::size_t offset = HEADER_SIZE;
  for (const auto &[page_id, page] : pages) {
    if (page_id != page->Id()) {
      return Err(Status::InvalidArgument("invalid WAL page ID"));
    }
    little_endian::PutU32(bytes, offset, page_id);
    std::ranges::copy(page->Bytes(), bytes.begin() + offset + sizeof(PageId));
    AddFrame(crc, page_id, page->Checksum());
    offset += FRAME_SIZE;
  }
  little_endian::PutU32(bytes, offset, crc.Finish());
  return bytes;
}

auto DecodeWal(std::span<const char> bytes) -> Result<PageMap> {
  PageMap pages;
  if (bytes.empty()) {
    return pages;
  }
  if (bytes.size() < WAL_HEADER_SIZE ||
      !std::ranges::equal(MAGIC, bytes.first(MAGIC.size())) ||
      little_endian::GetU32(bytes, HEADER_CRC_OFFSET) !=
          Crc32(bytes.first(HEADER_CRC_OFFSET))) {
    return Err(Status::Corruption("invalid WAL header"));
  }
  const auto salt = little_endian::GetU32(bytes, SALT_OFFSET);
  bytes = bytes.subspan(WAL_HEADER_SIZE);

  while (bytes.size() >= HEADER_SIZE &&
         little_endian::GetU32(bytes, 0) == salt) {
    auto size = WalRecordSize(little_endian::GetU32(bytes, 4));
    if (!size || *size > bytes.size()) {
      break;
    }
    const auto record = bytes.first(*size);
    const std::size_t crc_offset = record.size() - CRC_SIZE;
    Crc32Accumulator crc;
    crc.Update(record.first(HEADER_SIZE));
    std::vector<std::shared_ptr<Page>> frames;
    for (std::size_t offset = HEADER_SIZE; offset < crc_offset;
         offset += FRAME_SIZE) {
      const auto page_id = little_endian::GetU32(record, offset);
      if (!ValidDataPageId(page_id)) {
        break;
      }
      PageBytes page;
      std::ranges::copy(record.subspan(offset + sizeof(PageId), PAGE_SIZE),
                        page.begin());
      auto decoded = DecodePage(page_id, page);
      if (!decoded) {
        break;
      }
      AddFrame(crc, page_id, decoded->Checksum());
      frames.push_back(std::make_shared<Page>(std::move(*decoded)));
    }
    if (frames.size() != (crc_offset - HEADER_SIZE) / FRAME_SIZE ||
        little_endian::GetU32(record, crc_offset) != crc.Finish()) {
      break;
    }

    for (auto &page : frames) {
      const auto page_id = page->Id();
      pages.insert_or_assign(page_id, std::move(page));
    }
    bytes = bytes.subspan(record.size());
  }
  return pages;
}

} // namespace tinydb::storage
