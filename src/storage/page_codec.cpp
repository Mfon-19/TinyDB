#include "storage/page_codec.h"

#include "storage/encoding.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb::storage {
namespace {

constexpr std::size_t ALLOCATOR_NEXT_FREE_OFFSET = data_page_offset::HEADER_BYTES;
constexpr std::uint16_t ALLOCATOR_PAYLOAD_BYTES = sizeof(page_id_t);

constexpr std::size_t OVERFLOW_TOTAL_BYTES_OFFSET = data_page_offset::HEADER_BYTES;
constexpr std::size_t OVERFLOW_NEXT_PAGE_OFFSET = OVERFLOW_TOTAL_BYTES_OFFSET + sizeof(std::uint64_t);
constexpr std::size_t OVERFLOW_DATA_BYTES_OFFSET = OVERFLOW_NEXT_PAGE_OFFSET + sizeof(page_id_t);
constexpr std::size_t OVERFLOW_DATA_OFFSET = OVERFLOW_DATA_BYTES_OFFSET + sizeof(std::uint16_t);

auto ChecksumPage(std::span<const std::byte> input) -> std::uint32_t {
  auto page = std::array<std::byte, PAGE_SIZE>{};
  std::ranges::copy(input, page.begin());
  std::ranges::fill(page.begin() + static_cast<std::ptrdiff_t>(data_page_offset::CHECKSUM),
                    page.begin() + static_cast<std::ptrdiff_t>(data_page_offset::CHECKSUM + sizeof(std::uint32_t)),
                    std::byte{0});
  return Crc32(page);
}

auto IsKnownType(DataPageType type) -> bool {
  switch (type) {
    case DataPageType::Leaf:
    case DataPageType::Internal:
    case DataPageType::Allocator:
    case DataPageType::Overflow:
      return true;
  }
  return false;
}

}  // namespace

auto InitializeDataPage(std::span<std::byte> page, DataPageType type, page_id_t page_id, std::uint64_t page_lsn,
                        std::uint16_t payload_bytes) -> Status {
  if (page.size() != PAGE_SIZE) {
    return Status::InvalidArgument("data page is not exactly one page");
  }
  if (!IsKnownType(type)) {
    return Status::InvalidArgument("unknown data page type");
  }
  if (page_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("data page ID overlaps the superblocks");
  }
  if (payload_bytes > PAGE_SIZE - data_page_offset::HEADER_BYTES) {
    return Status::InvalidArgument("data-page payload exceeds one page");
  }

  std::ranges::fill(page, std::byte{0});
  const auto encoded = PutBytes(page, data_page_offset::MAGIC, DATA_PAGE_MAGIC) &&
                       PutLittleEndian(page, data_page_offset::TYPE, static_cast<std::uint16_t>(type)) &&
                       PutLittleEndian(page, data_page_offset::FORMAT_VERSION, DATA_PAGE_FORMAT_VERSION) &&
                       PutLittleEndian(page, data_page_offset::PAGE_ID, page_id) &&
                       PutLittleEndian(page, data_page_offset::PAGE_LSN, page_lsn) &&
                       PutLittleEndian(page, data_page_offset::PAYLOAD_BYTES, payload_bytes) &&
                       PutLittleEndian(page, data_page_offset::FLAGS, std::uint16_t{0}) &&
                       PutLittleEndian(page, data_page_offset::CHECKSUM, std::uint32_t{0});
  return encoded ? Status{} : Status::Corruption("internal data-page layout exceeds one page");
}

auto FinalizeDataPage(std::span<std::byte> page) -> Status {
  if (page.size() != PAGE_SIZE) {
    return Status::InvalidArgument("data page is not exactly one page");
  }
  if (!PutLittleEndian(page, data_page_offset::CHECKSUM, std::uint32_t{0}) ||
      !PutLittleEndian(page, data_page_offset::CHECKSUM, ChecksumPage(page))) {
    return Status::Corruption("data-page checksum field exceeds one page");
  }
  return {};
}

auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader> {
  if (page.size() != PAGE_SIZE) {
    return std::unexpected(Status::Corruption("data page is not exactly one page"));
  }
  if (!std::ranges::equal(DATA_PAGE_MAGIC, page.subspan(data_page_offset::MAGIC, DATA_PAGE_MAGIC.size()))) {
    return std::unexpected(Status::Corruption("unrecognized data-page magic"));
  }

  const auto stored_checksum = GetLittleEndian<std::uint32_t>(page, data_page_offset::CHECKSUM);
  if (!stored_checksum || *stored_checksum != ChecksumPage(page)) {
    return std::unexpected(Status::Corruption("data-page checksum mismatch"));
  }

  const auto raw_type = GetLittleEndian<std::uint16_t>(page, data_page_offset::TYPE);
  const auto version = GetLittleEndian<std::uint16_t>(page, data_page_offset::FORMAT_VERSION);
  const auto page_id = GetLittleEndian<page_id_t>(page, data_page_offset::PAGE_ID);
  const auto page_lsn = GetLittleEndian<std::uint64_t>(page, data_page_offset::PAGE_LSN);
  const auto payload_bytes = GetLittleEndian<std::uint16_t>(page, data_page_offset::PAYLOAD_BYTES);
  const auto flags = GetLittleEndian<std::uint16_t>(page, data_page_offset::FLAGS);
  if (!raw_type || !version || !page_id || !page_lsn || !payload_bytes || !flags) {
    return std::unexpected(Status::Corruption("truncated data-page header"));
  }
  const auto type = static_cast<DataPageType>(*raw_type);
  if (*version != DATA_PAGE_FORMAT_VERSION) {
    return std::unexpected(Status::UnsupportedFormat("unsupported data-page format version"));
  }
  if (!IsKnownType(type)) {
    return std::unexpected(Status::Corruption("unknown data-page type"));
  }
  if (*page_id != expected_page_id || *page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::Corruption("data-page ID does not match its file position"));
  }
  if (*payload_bytes > PAGE_SIZE - data_page_offset::HEADER_BYTES || *flags != 0) {
    return std::unexpected(Status::Corruption("invalid data-page length or flags"));
  }
  return DataPageHeader{
      .type = type, .page_id = *page_id, .page_lsn = *page_lsn, .payload_bytes = *payload_bytes, .flags = *flags};
}

auto EncodeAllocatorPage(page_id_t page_id, std::uint64_t page_lsn,
                         page_id_t next_free) -> Result<std::array<char, PAGE_SIZE>> {
  if (next_free != HEADER_PAGE_ID && next_free < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("allocator link overlaps the superblocks"));
  }
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status = InitializeDataPage(bytes, DataPageType::Allocator, page_id, page_lsn, ALLOCATOR_PAYLOAD_BYTES);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (!PutLittleEndian(bytes, ALLOCATOR_NEXT_FREE_OFFSET, next_free)) {
    return std::unexpected(Status::Corruption("allocator page layout exceeds one page"));
  }
  if (auto status = FinalizeDataPage(bytes); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return output;
}

auto DecodeAllocatorPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<AllocatorPage> {
  const auto header = DecodeDataPageHeader(page, expected_page_id);
  if (!header) {
    return std::unexpected(header.error());
  }
  if (header->type != DataPageType::Allocator || header->payload_bytes != ALLOCATOR_PAYLOAD_BYTES) {
    return std::unexpected(Status::Corruption("page is not allocator metadata"));
  }
  const auto next_free = GetLittleEndian<page_id_t>(page, ALLOCATOR_NEXT_FREE_OFFSET);
  if (!next_free || (*next_free != HEADER_PAGE_ID && *next_free < FIRST_DATA_PAGE_ID)) {
    return std::unexpected(Status::Corruption("invalid allocator page link"));
  }
  return AllocatorPage{.next_free = *next_free};
}

auto EncodeOverflowPage(page_id_t page_id, std::uint64_t page_lsn, std::uint64_t total_value_bytes,
                        page_id_t next_page_id,
                        std::span<const std::byte> payload) -> Result<std::array<char, PAGE_SIZE>> {
  if (payload.size() > PAGE_SIZE - OVERFLOW_DATA_OFFSET) {
    return std::unexpected(Status::InvalidArgument("overflow payload exceeds one page"));
  }
  if (next_page_id != HEADER_PAGE_ID && next_page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("overflow link overlaps the superblocks"));
  }
  const auto payload_bytes =
      static_cast<std::uint16_t>(OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES + payload.size());
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status = InitializeDataPage(bytes, DataPageType::Overflow, page_id, page_lsn, payload_bytes); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  const auto encoded = PutLittleEndian(bytes, OVERFLOW_TOTAL_BYTES_OFFSET, total_value_bytes) &&
                       PutLittleEndian(bytes, OVERFLOW_NEXT_PAGE_OFFSET, next_page_id) &&
                       PutLittleEndian(bytes, OVERFLOW_DATA_BYTES_OFFSET, static_cast<std::uint16_t>(payload.size())) &&
                       PutBytes(bytes, OVERFLOW_DATA_OFFSET, payload);
  if (!encoded) {
    return std::unexpected(Status::Corruption("overflow page layout exceeds one page"));
  }
  if (auto status = FinalizeDataPage(bytes); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return output;
}

auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage> {
  const auto header = DecodeDataPageHeader(page, expected_page_id);
  if (!header) {
    return std::unexpected(header.error());
  }
  if (header->type != DataPageType::Overflow ||
      header->payload_bytes < OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES) {
    return std::unexpected(Status::Corruption("page is not an overflow page"));
  }
  const auto total = GetLittleEndian<std::uint64_t>(page, OVERFLOW_TOTAL_BYTES_OFFSET);
  const auto next = GetLittleEndian<page_id_t>(page, OVERFLOW_NEXT_PAGE_OFFSET);
  const auto data_bytes = GetLittleEndian<std::uint16_t>(page, OVERFLOW_DATA_BYTES_OFFSET);
  if (!total || !next || !data_bytes || *data_bytes > PAGE_SIZE - OVERFLOW_DATA_OFFSET ||
      header->payload_bytes != OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES + *data_bytes ||
      (*next != HEADER_PAGE_ID && *next < FIRST_DATA_PAGE_ID)) {
    return std::unexpected(Status::Corruption("invalid overflow page lengths or link"));
  }
  auto result = OverflowPage{
      .total_value_bytes = *total,
      .next_page_id = *next,
      .payload = {},
  };
  result.payload.assign(page.begin() + static_cast<std::ptrdiff_t>(OVERFLOW_DATA_OFFSET),
                        page.begin() + static_cast<std::ptrdiff_t>(OVERFLOW_DATA_OFFSET + *data_bytes));
  return result;
}

}  // namespace tinydb::storage
