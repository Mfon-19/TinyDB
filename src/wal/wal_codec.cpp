#include "wal/wal_codec.h"

#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace tinydb::wal_format {
namespace {

static_assert(COMMIT_STATE_RESERVED_OFFSET + sizeof(std::uint32_t) <= COMMIT_STATE_PAYLOAD_BYTES);
static_assert(header_offset::ENCODED_BYTES <= HEADER_BYTES);
static_assert(record_offset::PAYLOAD == RECORD_HEADER_BYTES);

auto NonzeroUuid(const DatabaseUuid &uuid) -> bool {
  return std::ranges::any_of(uuid, [](std::byte byte) { return byte != std::byte{0}; });
}

auto KnownRecordType(RecordType type) -> bool {
  return type == RecordType::PageImage || type == RecordType::CommitState;
}

auto ValidState(const txn::DatabaseState &state) -> bool {
  const auto valid_reference = [&](page_id_t page_id) {
    return page_id == HEADER_PAGE_ID || (page_id >= FIRST_DATA_PAGE_ID && page_id < state.logical_page_count);
  };
  return state.visible_lsn != 0 && state.logical_page_count >= FIRST_DATA_PAGE_ID &&
         valid_reference(state.root_page_id) && valid_reference(state.allocator_root_page_id) &&
         state.checkpoint_lsn <= state.visible_lsn;
}

// Validates the final database state and encodes the terminal CommitState
// payload. The payload also records the number of page images.
auto EncodeCommitState(const txn::DatabaseState &state,
                       std::size_t page_image_count) -> Result<std::array<std::byte, COMMIT_STATE_PAYLOAD_BYTES>> {
  if (!ValidState(state) || page_image_count == 0 || page_image_count > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(Status::InvalidArgument("invalid WAL commit state"));
  }
  auto payload = std::array<std::byte, COMMIT_STATE_PAYLOAD_BYTES>{};
  storage::PutLittleEndianUnchecked(payload, COMMIT_STATE_ROOT_OFFSET, state.root_page_id);
  storage::PutLittleEndianUnchecked(payload, COMMIT_STATE_ALLOCATOR_ROOT_OFFSET, state.allocator_root_page_id);
  storage::PutLittleEndianUnchecked(payload, COMMIT_STATE_LOGICAL_PAGE_COUNT_OFFSET, state.logical_page_count);
  storage::PutLittleEndianUnchecked(payload, COMMIT_STATE_PAGE_IMAGE_COUNT_OFFSET,
                                    static_cast<std::uint32_t>(page_image_count));
  return payload;
}

// Decodes and validates a terminal CommitState payload. The result contains the
// final database state and the expected number of page images.
auto DecodeCommitState(std::span<const std::byte> payload,
                       std::uint64_t commit_lsn) -> Result<std::pair<txn::DatabaseState, std::uint32_t>> {
  if (payload.size() != COMMIT_STATE_PAYLOAD_BYTES) {
    return std::unexpected(Status::Corruption("WAL commit-state record has the wrong length"));
  }
  const auto root = storage::GetLittleEndianUnchecked<page_id_t>(payload, COMMIT_STATE_ROOT_OFFSET);
  const auto allocator = storage::GetLittleEndianUnchecked<page_id_t>(payload, COMMIT_STATE_ALLOCATOR_ROOT_OFFSET);
  const auto logical_page_count =
      storage::GetLittleEndianUnchecked<page_id_t>(payload, COMMIT_STATE_LOGICAL_PAGE_COUNT_OFFSET);
  const auto page_image_count =
      storage::GetLittleEndianUnchecked<std::uint32_t>(payload, COMMIT_STATE_PAGE_IMAGE_COUNT_OFFSET);
  const auto reserved = storage::GetLittleEndianUnchecked<std::uint32_t>(payload, COMMIT_STATE_RESERVED_OFFSET);
  if (page_image_count == 0 || reserved != 0) {
    return std::unexpected(Status::Corruption("invalid WAL commit-state fields"));
  }
  auto state = txn::DatabaseState{
      .root_page_id = root,
      .allocator_root_page_id = allocator,
      .logical_page_count = logical_page_count,
      .visible_lsn = commit_lsn,
      .checkpoint_lsn = 0,
  };
  if (!ValidState(state)) {
    return std::unexpected(Status::Corruption("invalid WAL database state"));
  }
  return std::pair{state, page_image_count};
}

// Reuses a validated page checksum to avoid a second scan of the page bytes.
// Without validated_header, record encoding computes the complete record CRC.
auto SealedPageCrc(const PageImageView &page) -> std::optional<std::uint32_t> {
  if (page.validated_header == nullptr) {
    return std::nullopt;
  }
  const auto bytes = std::as_bytes(page.bytes);
  const auto checksum = storage::GetLittleEndianUnchecked<std::uint32_t>(bytes, storage::data_page_offset::CHECKSUM);
  constexpr auto zero_checksum = std::array<std::byte, sizeof(std::uint32_t)>{};
  return Crc32Replace(checksum, zero_checksum,
                      bytes.subspan(storage::data_page_offset::CHECKSUM, sizeof(std::uint32_t)),
                      PAGE_SIZE - storage::data_page_offset::CHECKSUM - sizeof(std::uint32_t));
}

// Appends one self-framing record to destination. The callback writes the
// payload. An optional payload CRC avoids a second payload scan.
template <typename EncodePayload>
auto AppendEncodedRecord(std::vector<char> &destination, RecordType type, std::uint64_t lsn, std::size_t payload_bytes,
                         std::optional<std::uint32_t> payload_crc, EncodePayload encode_payload) -> Status {
  if (!KnownRecordType(type) || lsn == 0 ||
      payload_bytes > std::numeric_limits<std::uint32_t>::max() - RECORD_HEADER_BYTES) {
    return Status::InvalidArgument("invalid WAL record metadata");
  }
  const auto total_bytes = RECORD_HEADER_BYTES + payload_bytes;
  const auto offset = destination.size();
  destination.resize(offset + total_bytes, 0);
  auto bytes = std::as_writable_bytes(std::span{destination}).subspan(offset, total_bytes);
  storage::PutLittleEndianUnchecked(bytes, record_offset::TOTAL_BYTES, static_cast<std::uint32_t>(total_bytes));
  storage::PutLittleEndianUnchecked(bytes, record_offset::TYPE, static_cast<std::uint16_t>(type));
  storage::PutLittleEndianUnchecked(bytes, record_offset::FLAGS, std::uint16_t{0});
  storage::PutLittleEndianUnchecked(bytes, record_offset::LSN, lsn);
  storage::PutLittleEndianUnchecked(bytes, record_offset::CHECKSUM, std::uint32_t{0});
  storage::PutLittleEndianUnchecked(bytes, record_offset::RESERVED, std::uint32_t{0});
  encode_payload(bytes.subspan(record_offset::PAYLOAD, payload_bytes));
  const auto checksum = payload_crc
                            ? Crc32Combine(Crc32(bytes.first(record_offset::PAYLOAD)), *payload_crc, payload_bytes)
                            : Crc32WithZeroedU32(bytes, record_offset::CHECKSUM);
  storage::PutLittleEndianUnchecked(bytes, record_offset::CHECKSUM, checksum);
  return {};
}

// Appends one record whose payload already exists as a byte span. The general
// encoder computes the record CRC from the complete encoded record.
auto AppendRecord(std::vector<char> &destination, RecordType type, std::uint64_t lsn,
                  std::span<const std::byte> payload) -> Status {
  return AppendEncodedRecord(destination, type, lsn, payload.size(), std::nullopt,
                             [payload](std::span<std::byte> destination_payload) {
                               storage::PutBytesUnchecked(destination_payload, 0, payload);
                             });
}

}  // namespace

// Validates the logical header and encodes the fixed 64-byte WAL file header.
// The function writes the CRC after it writes all other fields.
auto EncodeHeader(const Header &header) -> Result<std::vector<char>> {
  if (!NonzeroUuid(header.database_uuid) || header.starting_lsn == 0) {
    return std::unexpected(Status::InvalidArgument("invalid WAL header identity or LSN"));
  }
  if ((header.required_features & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("unsupported required WAL feature"));
  }

  auto output = std::vector<char>(HEADER_BYTES, 0);
  auto bytes = std::as_writable_bytes(std::span{output});
  storage::PutBytesUnchecked(bytes, header_offset::MAGIC, MAGIC);
  storage::PutLittleEndianUnchecked(bytes, header_offset::FORMAT_MAJOR, FORMAT_MAJOR);
  storage::PutLittleEndianUnchecked(bytes, header_offset::FORMAT_MINOR, FORMAT_MINOR);
  storage::PutLittleEndianUnchecked(bytes, header_offset::HEADER_BYTES, static_cast<std::uint32_t>(HEADER_BYTES));
  storage::PutLittleEndianUnchecked(bytes, header_offset::REQUIRED_FEATURES, header.required_features);
  storage::PutLittleEndianUnchecked(bytes, header_offset::OPTIONAL_FEATURES, header.optional_features);
  storage::PutBytesUnchecked(bytes, header_offset::DATABASE_UUID, header.database_uuid);
  storage::PutLittleEndianUnchecked(bytes, header_offset::STARTING_LSN, header.starting_lsn);
  storage::PutLittleEndianUnchecked(bytes, header_offset::CHECKSUM, std::uint32_t{0});
  storage::PutLittleEndianUnchecked(bytes, header_offset::CHECKSUM, Crc32WithZeroedU32(bytes, header_offset::CHECKSUM));
  return output;
}

// Validates and decodes one complete 64-byte WAL file header. The function
// rejects incompatible formats, checksum errors, invalid identities, and
// nonzero reserved bytes.
auto DecodeHeader(std::span<const std::byte> bytes) -> Result<Header> {
  if (bytes.size() != HEADER_BYTES) {
    return std::unexpected(Status::Corruption("WAL header has the wrong length"));
  }
  if (!std::ranges::equal(MAGIC, bytes.subspan(header_offset::MAGIC, MAGIC.size()))) {
    return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB WAL magic"));
  }
  const auto checksum = storage::GetLittleEndianUnchecked<std::uint32_t>(bytes, header_offset::CHECKSUM);
  if (checksum != Crc32WithZeroedU32(bytes, header_offset::CHECKSUM)) {
    return std::unexpected(Status::Corruption("WAL header checksum mismatch"));
  }
  const auto major = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, header_offset::FORMAT_MAJOR);
  const auto minor = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, header_offset::FORMAT_MINOR);
  const auto encoded_header_bytes =
      storage::GetLittleEndianUnchecked<std::uint32_t>(bytes, header_offset::HEADER_BYTES);
  const auto required = storage::GetLittleEndianUnchecked<std::uint64_t>(bytes, header_offset::REQUIRED_FEATURES);
  const auto optional = storage::GetLittleEndianUnchecked<std::uint64_t>(bytes, header_offset::OPTIONAL_FEATURES);
  const auto starting_lsn = storage::GetLittleEndianUnchecked<std::uint64_t>(bytes, header_offset::STARTING_LSN);
  if (major != FORMAT_MAJOR || minor > FORMAT_MINOR || encoded_header_bytes != HEADER_BYTES ||
      (required & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("unsupported TinyDB WAL version or features"));
  }
  auto result = Header{};
  storage::GetBytesUnchecked(bytes, header_offset::DATABASE_UUID, result.database_uuid);
  result.starting_lsn = starting_lsn;
  result.required_features = required;
  result.optional_features = optional;
  if (!NonzeroUuid(result.database_uuid) || result.starting_lsn == 0) {
    return std::unexpected(Status::Corruption("invalid WAL identity or starting LSN"));
  }
  if (std::ranges::any_of(bytes.subspan(header_offset::ENCODED_BYTES),
                          [](std::byte byte) { return byte != std::byte{0}; })) {
    return std::unexpected(Status::Corruption("nonzero reserved WAL header bytes"));
  }
  return result;
}

// Encodes one checksummed WAL record into an owned byte vector.
auto EncodeRecord(RecordType type, std::uint64_t lsn, std::span<const std::byte> payload) -> Result<std::vector<char>> {
  auto output = std::vector<char>{};
  if (auto status = AppendRecord(output, type, lsn, payload); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return output;
}

// Validates one complete WAL record and returns a borrowed payload view. The
// input bytes must outlive the returned Record.
auto DecodeRecord(std::span<const std::byte> bytes) -> Result<Record> {
  if (bytes.size() < RECORD_HEADER_BYTES) {
    return std::unexpected(Status::Corruption("truncated WAL record header"));
  }
  const auto total = storage::GetLittleEndianUnchecked<std::uint32_t>(bytes, record_offset::TOTAL_BYTES);
  const auto raw_type = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, record_offset::TYPE);
  const auto flags = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, record_offset::FLAGS);
  const auto lsn = storage::GetLittleEndianUnchecked<std::uint64_t>(bytes, record_offset::LSN);
  const auto checksum = storage::GetLittleEndianUnchecked<std::uint32_t>(bytes, record_offset::CHECKSUM);
  const auto reserved = storage::GetLittleEndianUnchecked<std::uint32_t>(bytes, record_offset::RESERVED);
  if (total != bytes.size()) {
    return std::unexpected(Status::Corruption("invalid WAL record length"));
  }
  if (checksum != Crc32WithZeroedU32(bytes, record_offset::CHECKSUM)) {
    return std::unexpected(Status::Corruption("WAL record checksum mismatch"));
  }
  const auto type = static_cast<RecordType>(raw_type);
  if (!KnownRecordType(type) || flags != 0 || reserved != 0 || lsn == 0) {
    return std::unexpected(Status::Corruption("invalid WAL record metadata"));
  }
  return Record{.type = type, .lsn = lsn, .payload = bytes.subspan(record_offset::PAYLOAD)};
}

// Validates and encodes one complete WAL transaction. Ordered page images come
// first, and one terminal CommitState record comes last. Every record receives
// commit_lsn.
auto EncodeTransaction(std::uint64_t commit_lsn, std::span<const PageImageView> pages,
                       txn::DatabaseState state) -> Result<EncodedTransaction> {
  if (commit_lsn == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("WAL LSN space exhausted"));
  }
  if (commit_lsn == 0 || pages.empty() || pages.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(Status::InvalidArgument("invalid WAL transaction identity or page count"));
  }
  state.visible_lsn = commit_lsn;
  const auto commit_state = EncodeCommitState(state, pages.size());
  if (!commit_state) {
    return std::unexpected(commit_state.error());
  }

  auto output = std::vector<char>{};
  output.reserve(pages.size() * (RECORD_HEADER_BYTES + PAGE_IMAGE_PAYLOAD_BYTES) + RECORD_HEADER_BYTES +
                 COMMIT_STATE_PAYLOAD_BYTES);
  auto previous_page_id = HEADER_PAGE_ID;
  for (const auto &page : pages) {
    if (page.page_id < FIRST_DATA_PAGE_ID || page.page_id <= previous_page_id) {
      return std::unexpected(
          Status::InvalidArgument("WAL transaction contains unordered, invalid, or duplicate page IDs"));
    }
    previous_page_id = page.page_id;
    if (page.validated_header != nullptr) {
      if (page.validated_header->page_id != page.page_id || page.validated_header->page_lsn != commit_lsn) {
        return std::unexpected(Status::InvalidArgument("WAL page image disagrees with its seal proof"));
      }
    } else {
      const auto decoded = storage::DecodeDataPageHeader(std::as_bytes(page.bytes), page.page_id);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      if (decoded->page_lsn != commit_lsn) {
        return std::unexpected(Status::InvalidArgument("WAL page image has the wrong commit LSN"));
      }
    }
    if (auto status = AppendEncodedRecord(output, RecordType::PageImage, commit_lsn, PAGE_IMAGE_PAYLOAD_BYTES,
                                          SealedPageCrc(page),
                                          [&page](std::span<std::byte> payload) {
                                            storage::PutBytesUnchecked(payload, 0, std::as_bytes(page.bytes));
                                          });
        !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }
  if (auto status = AppendRecord(output, RecordType::CommitState, commit_lsn, *commit_state); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return EncodedTransaction{
      .commit_lsn = commit_lsn,
      .bytes = std::move(output),
  };
}

// Decodes one complete WAL transaction and validates its record order, common
// LSN, page images, terminal state, and page count. The result owns the decoded
// page images and database state.
auto DecodeTransaction(std::span<const std::byte> bytes,
                       std::uint64_t expected_commit_lsn) -> Result<DecodedTransaction> {
  if (bytes.empty() || expected_commit_lsn == 0) {
    return std::unexpected(Status::Corruption("empty WAL transaction"));
  }
  auto result = DecodedTransaction{
      .commit_lsn = expected_commit_lsn,
      .state = {},
      .pages = {},
  };
  auto previous_page_id = HEADER_PAGE_ID;
  auto offset = std::size_t{0};
  while (offset < bytes.size()) {
    const auto remaining = bytes.subspan(offset);
    if (remaining.size() < RECORD_HEADER_BYTES) {
      return std::unexpected(Status::Corruption("truncated WAL transaction record"));
    }
    const auto total = storage::GetLittleEndianUnchecked<std::uint32_t>(remaining, record_offset::TOTAL_BYTES);
    if (total < RECORD_HEADER_BYTES || total > remaining.size()) {
      return std::unexpected(Status::Corruption("invalid WAL transaction record length"));
    }
    const auto encoded_record = remaining.first(total);
    const auto record = DecodeRecord(encoded_record);
    if (!record) {
      return std::unexpected(record.error());
    }
    if (record->lsn != expected_commit_lsn) {
      return std::unexpected(Status::Corruption("WAL transaction records disagree on commit LSN"));
    }

    if (record->type == RecordType::PageImage) {
      if (record->payload.size() != PAGE_IMAGE_PAYLOAD_BYTES) {
        return std::unexpected(Status::Corruption("WAL page image has the wrong length"));
      }
      const auto page_id =
          storage::GetLittleEndianUnchecked<page_id_t>(record->payload, storage::data_page_offset::PAGE_ID);
      if (page_id < FIRST_DATA_PAGE_ID || page_id <= previous_page_id) {
        return std::unexpected(Status::Corruption("unordered, invalid, or duplicate WAL page-image ID"));
      }
      auto page = DecodedPageImage{.page_id = page_id, .bytes = {}};
      std::ranges::copy(record->payload, reinterpret_cast<std::byte *>(page.bytes.data()));
      const auto decoded = storage::DecodeDataPageHeader(std::as_bytes(std::span{page.bytes}), page_id);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      if (decoded->page_lsn != expected_commit_lsn) {
        return std::unexpected(Status::Corruption("WAL page image has the wrong commit LSN"));
      }
      previous_page_id = page_id;
      result.pages.push_back(page);
    } else {
      if (result.pages.empty() || offset + encoded_record.size() != bytes.size()) {
        return std::unexpected(Status::Corruption("WAL commit state does not terminate one complete transaction"));
      }
      const auto decoded = DecodeCommitState(record->payload, expected_commit_lsn);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      if (decoded->second != result.pages.size() || expected_commit_lsn == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(Status::Corruption("WAL commit state does not bind every page image"));
      }
      result.state = decoded->first;
      return result;
    }
    offset += encoded_record.size();
  }
  return std::unexpected(Status::Corruption("WAL transaction has no commit-state record"));
}

}  // namespace tinydb::wal_format
