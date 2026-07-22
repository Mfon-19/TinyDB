#include "storage/page_codec.h"

#include "storage/encoding.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace tinydb::storage {
namespace {

/*
** FREE-EXTENT PAYLOAD
**
**   32 next allocator page u64
**   40 extent count u16
**   42 reserved u16
**   44 reserved u32
**   48 repeated entries:
**        first page u64, page count u64, retirement LSN u64
**
** The outer common header carries the exact live payload length. The unused
** tail remains zero and participates in the page checksum.
*/
constexpr std::size_t EXTENT_NEXT_PAGE_OFFSET = data_page_offset::HEADER_BYTES;
constexpr std::size_t EXTENT_COUNT_OFFSET = EXTENT_NEXT_PAGE_OFFSET + sizeof(page_id_t);
constexpr std::size_t EXTENT_RESERVED_OFFSET = EXTENT_COUNT_OFFSET + sizeof(std::uint16_t);
constexpr std::size_t EXTENT_ENTRIES_OFFSET = EXTENT_RESERVED_OFFSET + sizeof(std::uint16_t) + sizeof(std::uint32_t);
constexpr std::size_t EXTENT_ENTRY_BYTES = sizeof(page_id_t) + sizeof(std::uint64_t) + sizeof(std::uint64_t);

/*
** OVERFLOW PAYLOAD
**
**   32 owning value ID u64 (the chain's first page)
**   40 chunk index u32
**   44 reserved u32
**   48 next overflow page u64
**   56 bytes stored in this page u16
**   58 reserved u16
**   60 payload bytes, followed by checksum-covered zero padding
*/
constexpr std::size_t OVERFLOW_OWNER_OFFSET = data_page_offset::HEADER_BYTES;
constexpr std::size_t OVERFLOW_CHUNK_INDEX_OFFSET = OVERFLOW_OWNER_OFFSET + sizeof(page_id_t);
constexpr std::size_t OVERFLOW_RESERVED32_OFFSET = OVERFLOW_CHUNK_INDEX_OFFSET + sizeof(std::uint32_t);
constexpr std::size_t OVERFLOW_NEXT_PAGE_OFFSET = OVERFLOW_RESERVED32_OFFSET + sizeof(std::uint32_t);
constexpr std::size_t OVERFLOW_DATA_BYTES_OFFSET = OVERFLOW_NEXT_PAGE_OFFSET + sizeof(page_id_t);
constexpr std::size_t OVERFLOW_RESERVED16_OFFSET = OVERFLOW_DATA_BYTES_OFFSET + sizeof(std::uint16_t);
constexpr std::size_t OVERFLOW_DATA_OFFSET = OVERFLOW_RESERVED16_OFFSET + sizeof(std::uint16_t);
static_assert(PAGE_SIZE - OVERFLOW_DATA_OFFSET == OVERFLOW_PAGE_PAYLOAD_BYTES);

auto ChecksumPage(std::span<const std::byte> input) -> std::uint32_t {
  // Hash a logical zero checksum field without copying or mutating a pinned
  // cache frame. Incremental CRC produces exactly the same persistent value.
  constexpr auto zero_checksum = std::array<std::byte, sizeof(std::uint32_t)>{};
  auto checksum = Crc32Accumulator{};
  checksum.Update(input.first(data_page_offset::CHECKSUM));
  checksum.Update(zero_checksum);
  checksum.Update(input.subspan(data_page_offset::CHECKSUM + sizeof(std::uint32_t)));
  return checksum.Finish();
}

auto IsKnownType(DataPageType type) -> bool {
  // Do not use a numeric range check: future format versions may intentionally
  // leave holes in the persisted type namespace.
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
  // This function accepts builder-owned memory, so bad arguments are caller
  // errors. DecodeDataPageHeader maps equivalent failures from disk to
  // Corruption/UnsupportedFormat instead.
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

  // Rebuilding from a zero page ensures removed records and old free-space
  // fragments do not survive into checksummed persistent bytes.
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
  // Always clear a previous checksum first. Builders may reuse a buffer that
  // previously held a valid page, and that old value must not influence CRC.
  if (!PutLittleEndian(page, data_page_offset::CHECKSUM, std::uint32_t{0}) ||
      !PutLittleEndian(page, data_page_offset::CHECKSUM, ChecksumPage(page))) {
    return Status::Corruption("data-page checksum field exceeds one page");
  }
  return {};
}

auto RewriteDataPageLsn(std::span<std::byte> page, page_id_t expected_page_id, std::uint64_t page_lsn) -> Status {
  const auto header = DecodeDataPageHeader(page, expected_page_id);
  if (!header) {
    return header.error();
  }
  if (!PutLittleEndian(page, data_page_offset::PAGE_LSN, page_lsn)) {
    return Status::Corruption("data-page LSN field exceeds one page");
  }
  return FinalizeDataPage(page);
}

auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader> {
  if (page.size() != PAGE_SIZE) {
    return std::unexpected(Status::Corruption("data page is not exactly one page"));
  }
  if (!std::ranges::equal(DATA_PAGE_MAGIC, page.subspan(data_page_offset::MAGIC, DATA_PAGE_MAGIC.size()))) {
    return std::unexpected(Status::Corruption("unrecognized data-page magic"));
  }

  // Authenticate the complete page before trusting type-specific offsets or
  // lengths. A valid checksum is necessary but semantic checks below are still
  // required because a correctly checksummed page can be misplaced or stale.
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
    // The physical-position check catches a page image written to the wrong
    // offset and stale bytes exposed by allocator reuse.
    return std::unexpected(Status::Corruption("data-page ID does not match its file position"));
  }
  if (*payload_bytes > PAGE_SIZE - data_page_offset::HEADER_BYTES || *flags != 0) {
    // Unknown flag semantics require a format version/feature bit; silently
    // accepting them would let this reader misinterpret a newer page.
    return std::unexpected(Status::Corruption("invalid data-page length or flags"));
  }
  return DataPageHeader{
      .type = type, .page_id = *page_id, .page_lsn = *page_lsn, .payload_bytes = *payload_bytes, .flags = *flags};
}

auto EncodeFreeExtentPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t next_page_id,
                          std::span<const FreeExtent> extents) -> Result<std::array<char, PAGE_SIZE>> {
  // Validate the complete logical page before emitting any bytes. In
  // particular, adjacent extents must already have been coalesced by the
  // transaction allocator so there is one canonical representation.
  if (next_page_id != HEADER_PAGE_ID && next_page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("free-extent link overlaps the superblocks"));
  }
  if (extents.size() > FREE_EXTENTS_PER_PAGE) {
    return std::unexpected(Status::InvalidArgument("too many extents for one allocator page"));
  }
  for (std::size_t index = 0; index < extents.size(); ++index) {
    const auto &extent = extents[index];
    if (extent.first_page_id < FIRST_DATA_PAGE_ID || extent.page_count == 0 ||
        extent.first_page_id > std::numeric_limits<page_id_t>::max() - extent.page_count) {
      return std::unexpected(Status::InvalidArgument("invalid free extent"));
    }
    if (index != 0) {
      const auto &previous = extents[index - 1];
      if (previous.first_page_id + previous.page_count >= extent.first_page_id) {
        return std::unexpected(Status::InvalidArgument("free extents overlap or are not coalesced"));
      }
    }
  }

  const auto payload_bytes = static_cast<std::uint16_t>(EXTENT_ENTRIES_OFFSET - data_page_offset::HEADER_BYTES +
                                                        extents.size() * EXTENT_ENTRY_BYTES);
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status = InitializeDataPage(bytes, DataPageType::Allocator, page_id, page_lsn, payload_bytes);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (!PutLittleEndian(bytes, EXTENT_NEXT_PAGE_OFFSET, next_page_id) ||
      !PutLittleEndian(bytes, EXTENT_COUNT_OFFSET, static_cast<std::uint16_t>(extents.size())) ||
      !PutLittleEndian(bytes, EXTENT_RESERVED_OFFSET, std::uint16_t{0}) ||
      !PutLittleEndian(bytes, EXTENT_RESERVED_OFFSET + sizeof(std::uint16_t), std::uint32_t{0})) {
    return std::unexpected(Status::Corruption("free-extent page header exceeds one page"));
  }
  for (std::size_t index = 0; index < extents.size(); ++index) {
    const auto offset = EXTENT_ENTRIES_OFFSET + index * EXTENT_ENTRY_BYTES;
    if (!PutLittleEndian(bytes, offset, extents[index].first_page_id) ||
        !PutLittleEndian(bytes, offset + sizeof(page_id_t), extents[index].page_count) ||
        !PutLittleEndian(bytes, offset + sizeof(page_id_t) + sizeof(std::uint64_t), extents[index].retire_lsn)) {
      return std::unexpected(Status::Corruption("free extent exceeds allocator page"));
    }
  }
  if (auto status = FinalizeDataPage(bytes); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return output;
}

namespace {

auto DecodeFreeExtentPayload(std::span<const std::byte> page, page_id_t expected_page_id,
                             const DataPageHeader &header) -> Result<FreeExtentPage> {
  if (page.size() != PAGE_SIZE || header.page_id != expected_page_id || header.type != DataPageType::Allocator ||
      header.payload_bytes < EXTENT_ENTRIES_OFFSET - data_page_offset::HEADER_BYTES) {
    return std::unexpected(Status::Corruption("page is not free-extent metadata"));
  }
  const auto next = GetLittleEndian<page_id_t>(page, EXTENT_NEXT_PAGE_OFFSET);
  const auto count = GetLittleEndian<std::uint16_t>(page, EXTENT_COUNT_OFFSET);
  const auto reserved16 = GetLittleEndian<std::uint16_t>(page, EXTENT_RESERVED_OFFSET);
  const auto reserved32 = GetLittleEndian<std::uint32_t>(page, EXTENT_RESERVED_OFFSET + sizeof(std::uint16_t));
  if (!next || !count || !reserved16 || !reserved32 || *reserved16 != 0 || *reserved32 != 0 ||
      (*next != HEADER_PAGE_ID && *next < FIRST_DATA_PAGE_ID) || *count > FREE_EXTENTS_PER_PAGE ||
      header.payload_bytes != EXTENT_ENTRIES_OFFSET - data_page_offset::HEADER_BYTES + *count * EXTENT_ENTRY_BYTES) {
    return std::unexpected(Status::Corruption("invalid free-extent page header"));
  }
  auto result = FreeExtentPage{.next_page_id = *next, .extents = {}};
  result.extents.reserve(*count);
  for (std::size_t index = 0; index < *count; ++index) {
    const auto offset = EXTENT_ENTRIES_OFFSET + index * EXTENT_ENTRY_BYTES;
    const auto first = GetLittleEndian<page_id_t>(page, offset);
    const auto pages = GetLittleEndian<std::uint64_t>(page, offset + sizeof(page_id_t));
    const auto retired = GetLittleEndian<std::uint64_t>(page, offset + sizeof(page_id_t) + sizeof(std::uint64_t));
    if (!first || !pages || !retired || *first < FIRST_DATA_PAGE_ID || *pages == 0 ||
        *first > std::numeric_limits<page_id_t>::max() - *pages) {
      return std::unexpected(Status::Corruption("invalid free extent"));
    }
    if (!result.extents.empty()) {
      const auto &previous = result.extents.back();
      if (previous.first_page_id + previous.page_count >= *first) {
        return std::unexpected(Status::Corruption("free extents overlap or are not coalesced"));
      }
    }
    result.extents.push_back(FreeExtent{.first_page_id = *first, .page_count = *pages, .retire_lsn = *retired});
  }
  return result;
}

}  // namespace

auto DecodeFreeExtentPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<FreeExtentPage> {
  // Common validation verifies checksum, identity, version, and generic
  // bounds before this decoder interprets the allocator-specific payload.
  const auto header = DecodeDataPageHeader(page, expected_page_id);
  if (!header) {
    return std::unexpected(header.error());
  }
  return DecodeFreeExtentPayload(page, expected_page_id, *header);
}

auto DecodeFreeExtentPage(std::span<const std::byte> page, page_id_t expected_page_id,
                          const DataPageHeader &validated_header) -> Result<FreeExtentPage> {
  return DecodeFreeExtentPayload(page, expected_page_id, validated_header);
}

auto EncodeOverflowPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t owner_value_id, std::uint32_t chunk_index,
                        page_id_t next_page_id,
                        std::span<const std::byte> payload) -> Result<std::array<char, PAGE_SIZE>> {
  if (payload.empty() || payload.size() > OVERFLOW_PAGE_PAYLOAD_BYTES) {
    return std::unexpected(Status::InvalidArgument("overflow payload exceeds one page"));
  }
  if (owner_value_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("overflow owner overlaps the superblocks"));
  }
  if (next_page_id != HEADER_PAGE_ID && next_page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("overflow link overlaps the superblocks"));
  }
  // Common payload_bytes includes overflow metadata plus live data, but not
  // the zero-filled tail of the physical page.
  const auto payload_bytes =
      static_cast<std::uint16_t>(OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES + payload.size());
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status = InitializeDataPage(bytes, DataPageType::Overflow, page_id, page_lsn, payload_bytes); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  const auto encoded = PutLittleEndian(bytes, OVERFLOW_OWNER_OFFSET, owner_value_id) &&
                       PutLittleEndian(bytes, OVERFLOW_CHUNK_INDEX_OFFSET, chunk_index) &&
                       PutLittleEndian(bytes, OVERFLOW_RESERVED32_OFFSET, std::uint32_t{0}) &&
                       PutLittleEndian(bytes, OVERFLOW_NEXT_PAGE_OFFSET, next_page_id) &&
                       PutLittleEndian(bytes, OVERFLOW_DATA_BYTES_OFFSET, static_cast<std::uint16_t>(payload.size())) &&
                       PutLittleEndian(bytes, OVERFLOW_RESERVED16_OFFSET, std::uint16_t{0}) &&
                       PutBytes(bytes, OVERFLOW_DATA_OFFSET, payload);
  if (!encoded) {
    return std::unexpected(Status::Corruption("overflow page layout exceeds one page"));
  }
  if (auto status = FinalizeDataPage(bytes); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return output;
}

namespace {

auto DecodeOverflowPayload(std::span<const std::byte> page, page_id_t expected_page_id,
                           const DataPageHeader &header) -> Result<OverflowPage> {
  if (page.size() != PAGE_SIZE || header.page_id != expected_page_id || header.type != DataPageType::Overflow ||
      header.payload_bytes < OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES) {
    return std::unexpected(Status::Corruption("page is not an overflow page"));
  }
  const auto owner = GetLittleEndian<page_id_t>(page, OVERFLOW_OWNER_OFFSET);
  const auto chunk_index = GetLittleEndian<std::uint32_t>(page, OVERFLOW_CHUNK_INDEX_OFFSET);
  const auto reserved32 = GetLittleEndian<std::uint32_t>(page, OVERFLOW_RESERVED32_OFFSET);
  const auto next = GetLittleEndian<page_id_t>(page, OVERFLOW_NEXT_PAGE_OFFSET);
  const auto data_bytes = GetLittleEndian<std::uint16_t>(page, OVERFLOW_DATA_BYTES_OFFSET);
  const auto reserved16 = GetLittleEndian<std::uint16_t>(page, OVERFLOW_RESERVED16_OFFSET);
  // Cross-check the inner data length against the common outer payload length.
  // Redundant lengths are useful only if inconsistent encodings are rejected.
  if (!owner || !chunk_index || !reserved32 || !next || !data_bytes || !reserved16 || *owner < FIRST_DATA_PAGE_ID ||
      *reserved32 != 0 || *reserved16 != 0 || *data_bytes == 0 || *data_bytes > OVERFLOW_PAGE_PAYLOAD_BYTES ||
      header.payload_bytes != OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES + *data_bytes ||
      (*next != HEADER_PAGE_ID && *next < FIRST_DATA_PAGE_ID)) {
    return std::unexpected(Status::Corruption("invalid overflow page lengths or link"));
  }
  auto result = OverflowPage{
      .page_lsn = header.page_lsn,
      .owner_value_id = *owner,
      .chunk_index = *chunk_index,
      .next_page_id = *next,
      .payload = page.subspan(OVERFLOW_DATA_OFFSET, *data_bytes),
  };
  return result;
}

}  // namespace

auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage> {
  const auto header = DecodeDataPageHeader(page, expected_page_id);
  if (!header) {
    return std::unexpected(header.error());
  }
  return DecodeOverflowPayload(page, expected_page_id, *header);
}

auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id,
                        const DataPageHeader &validated_header) -> Result<OverflowPage> {
  return DecodeOverflowPayload(page, expected_page_id, validated_header);
}

}  // namespace tinydb::storage
