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
static_assert(EXTENT_ENTRIES_OFFSET + FREE_EXTENTS_PER_PAGE * EXTENT_ENTRY_BYTES <= PAGE_SIZE);

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

auto DecodeDataPageHeaderFields(std::span<const std::byte> page, page_id_t expected_page_id,
                                bool verify_checksum) -> Result<DataPageHeader> {
  if (page.size() != PAGE_SIZE) {
    return std::unexpected(Status::Corruption("data page is not exactly one page"));
  }
  if (!std::ranges::equal(DATA_PAGE_MAGIC, page.subspan(data_page_offset::MAGIC, DATA_PAGE_MAGIC.size()))) {
    return std::unexpected(Status::Corruption("unrecognized data-page magic"));
  }

  if (verify_checksum) {
    const auto stored_checksum = GetLittleEndianUnchecked<std::uint32_t>(page, data_page_offset::CHECKSUM);
    if (stored_checksum != Crc32WithZeroedU32(page, data_page_offset::CHECKSUM)) {
      return std::unexpected(Status::Corruption("data-page checksum mismatch"));
    }
  }

  const auto type = static_cast<DataPageType>(GetLittleEndianUnchecked<std::uint16_t>(page, data_page_offset::TYPE));
  const auto version = GetLittleEndianUnchecked<std::uint16_t>(page, data_page_offset::FORMAT_VERSION);
  const auto page_id = GetLittleEndianUnchecked<page_id_t>(page, data_page_offset::PAGE_ID);
  const auto page_lsn = GetLittleEndianUnchecked<std::uint64_t>(page, data_page_offset::PAGE_LSN);
  const auto payload_bytes = GetLittleEndianUnchecked<std::uint16_t>(page, data_page_offset::PAYLOAD_BYTES);
  const auto flags = GetLittleEndianUnchecked<std::uint16_t>(page, data_page_offset::FLAGS);
  if (version != DATA_PAGE_FORMAT_VERSION) {
    return std::unexpected(Status::UnsupportedFormat("unsupported data-page format version"));
  }
  if (!IsKnownType(type)) {
    return std::unexpected(Status::Corruption("unknown data-page type"));
  }
  if (page_id != expected_page_id || page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::Corruption("data-page ID does not match its file position"));
  }
  if (payload_bytes > PAGE_SIZE - data_page_offset::HEADER_BYTES || flags != 0) {
    return std::unexpected(Status::Corruption("invalid data-page length or flags"));
  }
  return DataPageHeader{
      .type = type, .page_id = page_id, .page_lsn = page_lsn, .payload_bytes = payload_bytes, .flags = flags};
}

}  // namespace

void InitializeDataPage(std::span<std::byte, PAGE_SIZE> page, DataPageType type, page_id_t page_id,
                        std::uint64_t page_lsn, std::uint16_t payload_bytes) {
  // Rebuilding from a zero page ensures removed records and old free-space
  // fragments do not survive into checksummed persistent bytes.
  std::ranges::fill(page, std::byte{0});
  PutBytesUnchecked(page, data_page_offset::MAGIC, DATA_PAGE_MAGIC);
  PutLittleEndianUnchecked(page, data_page_offset::TYPE, static_cast<std::uint16_t>(type));
  PutLittleEndianUnchecked(page, data_page_offset::FORMAT_VERSION, DATA_PAGE_FORMAT_VERSION);
  PutLittleEndianUnchecked(page, data_page_offset::PAGE_ID, page_id);
  PutLittleEndianUnchecked(page, data_page_offset::PAGE_LSN, page_lsn);
  PutLittleEndianUnchecked(page, data_page_offset::PAYLOAD_BYTES, payload_bytes);
  PutLittleEndianUnchecked(page, data_page_offset::FLAGS, std::uint16_t{0});
  PutLittleEndianUnchecked(page, data_page_offset::CHECKSUM, std::uint32_t{0});
}

void FinalizeDataPage(std::span<std::byte, PAGE_SIZE> page) {
  // The calculation treats an existing checksum as zero without changing the
  // page before it stores the new value.
  PutLittleEndianUnchecked(page, data_page_offset::CHECKSUM, Crc32WithZeroedU32(page, data_page_offset::CHECKSUM));
}

auto RewriteDataPageLsn(std::span<std::byte, PAGE_SIZE> page, page_id_t expected_page_id,
                        std::uint64_t page_lsn) -> Result<DataPageHeader> {
  // This is the transaction-to-durability boundary. Private pages were either
  // copied from an authenticated frame or produced by an internal codec, so
  // verifying the provisional checksum immediately before replacing it would
  // only hash the same bytes twice. Semantic fields are still checked here,
  // and the returned proof stays attached to these immutable final bytes.
  auto header = DecodeDataPageHeaderFields(page, expected_page_id, false);
  if (!header) {
    return std::unexpected(header.error());
  }
  PutLittleEndianUnchecked(page, data_page_offset::PAGE_LSN, page_lsn);
  FinalizeDataPage(page);
  header->page_lsn = page_lsn;
  return *header;
}

auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader> {
  return DecodeDataPageHeaderFields(page, expected_page_id, true);
}

auto InitializeFreeExtentPage(std::span<std::byte, PAGE_SIZE> page, page_id_t page_id, std::uint64_t page_lsn,
                              page_id_t next_page_id, std::span<const FreeExtent> extents) -> Status {
  // Validate the complete logical page before emitting any bytes. In
  // particular, adjacent extents must already have been coalesced by the
  // transaction allocator so there is one canonical representation.
  if (page_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("free-extent page ID overlaps the superblocks");
  }
  if (next_page_id != HEADER_PAGE_ID && next_page_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("free-extent link overlaps the superblocks");
  }
  if (extents.size() > FREE_EXTENTS_PER_PAGE) {
    return Status::InvalidArgument("too many extents for one allocator page");
  }
  for (std::size_t index = 0; index < extents.size(); ++index) {
    const auto &extent = extents[index];
    if (extent.first_page_id < FIRST_DATA_PAGE_ID || extent.page_count == 0 ||
        extent.first_page_id > std::numeric_limits<page_id_t>::max() - extent.page_count) {
      return Status::InvalidArgument("invalid free extent");
    }
    if (index != 0) {
      const auto &previous = extents[index - 1];
      if (previous.first_page_id + previous.page_count >= extent.first_page_id) {
        return Status::InvalidArgument("free extents overlap or are not coalesced");
      }
    }
  }

  const auto payload_bytes = static_cast<std::uint16_t>(EXTENT_ENTRIES_OFFSET - data_page_offset::HEADER_BYTES +
                                                        extents.size() * EXTENT_ENTRY_BYTES);
  InitializeDataPage(page, DataPageType::Allocator, page_id, page_lsn, payload_bytes);
  PutLittleEndianUnchecked(page, EXTENT_NEXT_PAGE_OFFSET, next_page_id);
  PutLittleEndianUnchecked(page, EXTENT_COUNT_OFFSET, static_cast<std::uint16_t>(extents.size()));
  PutLittleEndianUnchecked(page, EXTENT_RESERVED_OFFSET, std::uint16_t{0});
  PutLittleEndianUnchecked(page, EXTENT_RESERVED_OFFSET + sizeof(std::uint16_t), std::uint32_t{0});
  for (std::size_t index = 0; index < extents.size(); ++index) {
    const auto offset = EXTENT_ENTRIES_OFFSET + index * EXTENT_ENTRY_BYTES;
    PutLittleEndianUnchecked(page, offset, extents[index].first_page_id);
    PutLittleEndianUnchecked(page, offset + sizeof(page_id_t), extents[index].page_count);
    PutLittleEndianUnchecked(page, offset + sizeof(page_id_t) + sizeof(std::uint64_t), extents[index].retire_lsn);
  }
  return {};
}

auto EncodeFreeExtentPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t next_page_id,
                          std::span<const FreeExtent> extents) -> Result<std::array<char, PAGE_SIZE>> {
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status = InitializeFreeExtentPage(bytes, page_id, page_lsn, next_page_id, extents); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  FinalizeDataPage(bytes);
  return output;
}

namespace {

auto DecodeFreeExtentPayload(std::span<const std::byte> page, page_id_t expected_page_id,
                             const DataPageHeader &header) -> Result<FreeExtentPage> {
  if (page.size() != PAGE_SIZE || header.page_id != expected_page_id || header.type != DataPageType::Allocator ||
      header.payload_bytes < EXTENT_ENTRIES_OFFSET - data_page_offset::HEADER_BYTES) {
    return std::unexpected(Status::Corruption("page is not free-extent metadata"));
  }
  const auto next = GetLittleEndianUnchecked<page_id_t>(page, EXTENT_NEXT_PAGE_OFFSET);
  const auto count = GetLittleEndianUnchecked<std::uint16_t>(page, EXTENT_COUNT_OFFSET);
  const auto reserved16 = GetLittleEndianUnchecked<std::uint16_t>(page, EXTENT_RESERVED_OFFSET);
  const auto reserved32 = GetLittleEndianUnchecked<std::uint32_t>(page, EXTENT_RESERVED_OFFSET + sizeof(std::uint16_t));
  if (reserved16 != 0 || reserved32 != 0 || (next != HEADER_PAGE_ID && next < FIRST_DATA_PAGE_ID) ||
      count > FREE_EXTENTS_PER_PAGE ||
      header.payload_bytes != EXTENT_ENTRIES_OFFSET - data_page_offset::HEADER_BYTES + count * EXTENT_ENTRY_BYTES) {
    return std::unexpected(Status::Corruption("invalid free-extent page header"));
  }
  auto result = FreeExtentPage{.next_page_id = next, .extents = {}};
  result.extents.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = EXTENT_ENTRIES_OFFSET + index * EXTENT_ENTRY_BYTES;
    const auto first = GetLittleEndianUnchecked<page_id_t>(page, offset);
    const auto pages = GetLittleEndianUnchecked<std::uint64_t>(page, offset + sizeof(page_id_t));
    const auto retired =
        GetLittleEndianUnchecked<std::uint64_t>(page, offset + sizeof(page_id_t) + sizeof(std::uint64_t));
    if (first < FIRST_DATA_PAGE_ID || pages == 0 || first > std::numeric_limits<page_id_t>::max() - pages) {
      return std::unexpected(Status::Corruption("invalid free extent"));
    }
    if (!result.extents.empty()) {
      const auto &previous = result.extents.back();
      if (previous.first_page_id + previous.page_count >= first) {
        return std::unexpected(Status::Corruption("free extents overlap or are not coalesced"));
      }
    }
    result.extents.push_back(FreeExtent{.first_page_id = first, .page_count = pages, .retire_lsn = retired});
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

auto InitializeOverflowPage(std::span<std::byte, PAGE_SIZE> page, page_id_t page_id, std::uint64_t page_lsn,
                            page_id_t owner_value_id, std::uint32_t chunk_index, page_id_t next_page_id,
                            std::span<const std::byte> payload) -> Status {
  if (payload.empty() || payload.size() > OVERFLOW_PAGE_PAYLOAD_BYTES) {
    return Status::InvalidArgument("overflow payload exceeds one page");
  }
  if (page_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("overflow page ID overlaps the superblocks");
  }
  if (owner_value_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("overflow owner overlaps the superblocks");
  }
  if (next_page_id != HEADER_PAGE_ID && next_page_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("overflow link overlaps the superblocks");
  }
  // Common payload_bytes includes overflow metadata plus live data, but not
  // the zero-filled tail of the physical page.
  const auto payload_bytes =
      static_cast<std::uint16_t>(OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES + payload.size());
  InitializeDataPage(page, DataPageType::Overflow, page_id, page_lsn, payload_bytes);
  PutLittleEndianUnchecked(page, OVERFLOW_OWNER_OFFSET, owner_value_id);
  PutLittleEndianUnchecked(page, OVERFLOW_CHUNK_INDEX_OFFSET, chunk_index);
  PutLittleEndianUnchecked(page, OVERFLOW_RESERVED32_OFFSET, std::uint32_t{0});
  PutLittleEndianUnchecked(page, OVERFLOW_NEXT_PAGE_OFFSET, next_page_id);
  PutLittleEndianUnchecked(page, OVERFLOW_DATA_BYTES_OFFSET, static_cast<std::uint16_t>(payload.size()));
  PutLittleEndianUnchecked(page, OVERFLOW_RESERVED16_OFFSET, std::uint16_t{0});
  PutBytesUnchecked(page, OVERFLOW_DATA_OFFSET, payload);
  return {};
}

auto EncodeOverflowPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t owner_value_id, std::uint32_t chunk_index,
                        page_id_t next_page_id,
                        std::span<const std::byte> payload) -> Result<std::array<char, PAGE_SIZE>> {
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status =
          InitializeOverflowPage(bytes, page_id, page_lsn, owner_value_id, chunk_index, next_page_id, payload);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  FinalizeDataPage(bytes);
  return output;
}

namespace {

auto DecodeOverflowPayload(std::span<const std::byte> page, page_id_t expected_page_id,
                           const DataPageHeader &header) -> Result<OverflowPage> {
  if (page.size() != PAGE_SIZE || header.page_id != expected_page_id || header.type != DataPageType::Overflow ||
      header.payload_bytes < OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES) {
    return std::unexpected(Status::Corruption("page is not an overflow page"));
  }
  const auto owner = GetLittleEndianUnchecked<page_id_t>(page, OVERFLOW_OWNER_OFFSET);
  const auto chunk_index = GetLittleEndianUnchecked<std::uint32_t>(page, OVERFLOW_CHUNK_INDEX_OFFSET);
  const auto reserved32 = GetLittleEndianUnchecked<std::uint32_t>(page, OVERFLOW_RESERVED32_OFFSET);
  const auto next = GetLittleEndianUnchecked<page_id_t>(page, OVERFLOW_NEXT_PAGE_OFFSET);
  const auto data_bytes = GetLittleEndianUnchecked<std::uint16_t>(page, OVERFLOW_DATA_BYTES_OFFSET);
  const auto reserved16 = GetLittleEndianUnchecked<std::uint16_t>(page, OVERFLOW_RESERVED16_OFFSET);
  // Cross-check the inner data length against the common outer payload length.
  // Redundant lengths are useful only if inconsistent encodings are rejected.
  if (owner < FIRST_DATA_PAGE_ID || reserved32 != 0 || reserved16 != 0 || data_bytes == 0 ||
      data_bytes > OVERFLOW_PAGE_PAYLOAD_BYTES ||
      header.payload_bytes != OVERFLOW_DATA_OFFSET - data_page_offset::HEADER_BYTES + data_bytes ||
      (next != HEADER_PAGE_ID && next < FIRST_DATA_PAGE_ID)) {
    return std::unexpected(Status::Corruption("invalid overflow page lengths or link"));
  }
  auto result = OverflowPage{
      .page_lsn = header.page_lsn,
      .owner_value_id = owner,
      .chunk_index = chunk_index,
      .next_page_id = next,
      .payload = page.subspan(OVERFLOW_DATA_OFFSET, data_bytes),
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
