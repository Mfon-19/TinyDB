#include "wal/wal_codec.h"

#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "util/check.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace tinydb::wal_format {
namespace {

// Zero is the uninitialized sentinel shared with the database superblock
// codec, never a valid durable identity.
auto NonzeroUuid(const DatabaseUuid &uuid) -> bool {
  return std::ranges::any_of(uuid, [](std::byte byte) { return byte != std::byte{0}; });
}

auto ChecksumWithZeroedField(std::span<const std::byte> input, std::size_t checksum_offset) -> std::uint32_t {
  // Headers and records have different sizes, so use one generic helper. The
  // caller supplies a format-constant offset, never an offset read from disk.
  TINYDB_CHECK(checksum_offset + sizeof(std::uint32_t) <= input.size(), "checksum field exceeds encoded object");
  constexpr auto zero_checksum = std::array<std::byte, sizeof(std::uint32_t)>{};
  auto checksum = Crc32Accumulator{};
  checksum.Update(input.first(checksum_offset));
  checksum.Update(zero_checksum);
  checksum.Update(input.subspan(checksum_offset + zero_checksum.size()));
  return checksum.Finish();
}

// Enumerate accepted values explicitly so reserved numeric codes cannot be
// mistaken for records whose semantics this recovery implementation knows.
auto KnownRecordType(RecordType type) -> bool {
  return type == RecordType::PageImage || type == RecordType::DatabaseState || type == RecordType::Commit;
}

auto ValidState(const txn::DatabaseState &state) -> bool {
  const auto valid_reference = [&](page_id_t page_id) {
    return page_id == HEADER_PAGE_ID || (page_id >= FIRST_DATA_PAGE_ID && page_id < state.high_water_page_id);
  };
  return state.transaction_id != 0 && state.high_water_page_id >= FIRST_DATA_PAGE_ID &&
         valid_reference(state.root_page_id) && valid_reference(state.allocator_root_page_id) &&
         state.checkpoint_lsn <= state.visible_lsn;
}

auto EncodeDatabaseState(const txn::DatabaseState &state)
    -> Result<std::array<std::byte, DATABASE_STATE_PAYLOAD_BYTES>> {
  if (!ValidState(state)) {
    return std::unexpected(Status::InvalidArgument("invalid WAL database state"));
  }
  auto payload = std::array<std::byte, DATABASE_STATE_PAYLOAD_BYTES>{};
  const auto encoded =
      storage::PutLittleEndian(payload, DATABASE_STATE_ROOT_OFFSET, state.root_page_id) &&
      storage::PutLittleEndian(payload, DATABASE_STATE_ALLOCATOR_ROOT_OFFSET, state.allocator_root_page_id) &&
      storage::PutLittleEndian(payload, DATABASE_STATE_HIGH_WATER_OFFSET, state.high_water_page_id) &&
      storage::PutLittleEndian(payload, DATABASE_STATE_TRANSACTION_ID_OFFSET, state.transaction_id) &&
      storage::PutLittleEndian(payload, DATABASE_STATE_VISIBLE_LSN_OFFSET, state.visible_lsn) &&
      storage::PutLittleEndian(payload, DATABASE_STATE_CHECKPOINT_LSN_OFFSET, state.checkpoint_lsn);
  if (!encoded) {
    return std::unexpected(Status::Corruption("internal WAL database-state layout exceeds its buffer"));
  }
  return payload;
}

auto DecodeDatabaseState(std::span<const std::byte> payload) -> Result<txn::DatabaseState> {
  if (payload.size() != DATABASE_STATE_PAYLOAD_BYTES) {
    return std::unexpected(Status::Corruption("WAL database-state record has the wrong length"));
  }
  const auto root = storage::GetLittleEndian<page_id_t>(payload, DATABASE_STATE_ROOT_OFFSET);
  const auto allocator = storage::GetLittleEndian<page_id_t>(payload, DATABASE_STATE_ALLOCATOR_ROOT_OFFSET);
  const auto high_water = storage::GetLittleEndian<page_id_t>(payload, DATABASE_STATE_HIGH_WATER_OFFSET);
  const auto transaction_id = storage::GetLittleEndian<std::uint64_t>(payload, DATABASE_STATE_TRANSACTION_ID_OFFSET);
  const auto visible_lsn = storage::GetLittleEndian<std::uint64_t>(payload, DATABASE_STATE_VISIBLE_LSN_OFFSET);
  const auto checkpoint_lsn = storage::GetLittleEndian<std::uint64_t>(payload, DATABASE_STATE_CHECKPOINT_LSN_OFFSET);
  if (!root || !allocator || !high_water || !transaction_id || !visible_lsn || !checkpoint_lsn) {
    return std::unexpected(Status::Corruption("truncated WAL database-state fields"));
  }
  auto state = txn::DatabaseState{
      .root_page_id = *root,
      .allocator_root_page_id = *allocator,
      .high_water_page_id = *high_water,
      .transaction_id = *transaction_id,
      .visible_lsn = *visible_lsn,
      .checkpoint_lsn = *checkpoint_lsn,
  };
  if (!ValidState(state)) {
    return std::unexpected(Status::Corruption("invalid WAL database state"));
  }
  return state;
}

template <typename EncodePayload>
auto AppendEncodedRecord(std::vector<char> &destination, RecordType type, std::uint64_t transaction_id,
                         std::uint64_t lsn, std::uint32_t record_sequence, std::size_t payload_bytes,
                         EncodePayload encode_payload) -> Status {
  if (!KnownRecordType(type) || transaction_id == 0 || lsn == 0 ||
      payload_bytes > std::numeric_limits<std::uint32_t>::max() - RECORD_HEADER_BYTES) {
    return Status::InvalidArgument("invalid WAL record metadata");
  }
  const auto total_bytes = RECORD_HEADER_BYTES + payload_bytes;
  const auto offset = destination.size();
  destination.resize(offset + total_bytes, 0);
  auto bytes = std::as_writable_bytes(std::span{destination}).subspan(offset, total_bytes);
  const auto encoded =
      storage::PutLittleEndian(bytes, record_offset::TOTAL_BYTES, static_cast<std::uint32_t>(total_bytes)) &&
      storage::PutLittleEndian(bytes, record_offset::TYPE, static_cast<std::uint16_t>(type)) &&
      storage::PutLittleEndian(bytes, record_offset::FLAGS, std::uint16_t{0}) &&
      storage::PutLittleEndian(bytes, record_offset::TRANSACTION_ID, transaction_id) &&
      storage::PutLittleEndian(bytes, record_offset::LSN, lsn) &&
      storage::PutLittleEndian(bytes, record_offset::RECORD_SEQUENCE, record_sequence) &&
      storage::PutLittleEndian(bytes, record_offset::CHECKSUM, std::uint32_t{0}) &&
      storage::PutLittleEndian(bytes, record_offset::RESERVED, std::uint64_t{0}) &&
      encode_payload(bytes.subspan(record_offset::PAYLOAD, payload_bytes));
  if (!encoded || !storage::PutLittleEndian(bytes, record_offset::CHECKSUM,
                                            ChecksumWithZeroedField(bytes, record_offset::CHECKSUM))) {
    destination.resize(offset);
    return Status::Corruption("internal WAL record layout exceeds its buffer");
  }
  return {};
}

auto AppendRecord(std::vector<char> &destination, RecordType type, std::uint64_t transaction_id, std::uint64_t lsn,
                  std::uint32_t record_sequence, std::span<const std::byte> payload) -> Status {
  return AppendEncodedRecord(destination, type, transaction_id, lsn, record_sequence, payload.size(),
                             [payload](std::span<std::byte> destination_payload) {
                               return storage::PutBytes(destination_payload, 0, payload);
                             });
}

}  // namespace

auto EncodeHeader(const Header &header) -> Result<std::vector<char>> {
  if (!NonzeroUuid(header.database_uuid) || header.segment_id == 0 || header.starting_lsn == 0) {
    return std::unexpected(Status::InvalidArgument("invalid WAL header identity or LSN"));
  }
  if ((header.required_features & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("unsupported required WAL feature"));
  }

  // Zero initialization canonicalizes the reserved extension area and makes
  // its contents part of the checksum contract.
  auto output = std::vector<char>(HEADER_BYTES, 0);
  auto bytes = std::as_writable_bytes(std::span{output});
  const auto encoded =
      storage::PutBytes(bytes, header_offset::MAGIC, MAGIC) &&
      storage::PutLittleEndian(bytes, header_offset::FORMAT_MAJOR, FORMAT_MAJOR) &&
      storage::PutLittleEndian(bytes, header_offset::FORMAT_MINOR, FORMAT_MINOR) &&
      storage::PutLittleEndian(bytes, header_offset::HEADER_BYTES, static_cast<std::uint32_t>(HEADER_BYTES)) &&
      storage::PutLittleEndian(bytes, header_offset::REQUIRED_FEATURES, header.required_features) &&
      storage::PutLittleEndian(bytes, header_offset::OPTIONAL_FEATURES, header.optional_features) &&
      storage::PutBytes(bytes, header_offset::DATABASE_UUID, header.database_uuid) &&
      storage::PutLittleEndian(bytes, header_offset::SEGMENT_ID, header.segment_id) &&
      storage::PutLittleEndian(bytes, header_offset::STARTING_LSN, header.starting_lsn) &&
      storage::PutLittleEndian(bytes, header_offset::CHECKSUM, std::uint32_t{0});
  if (!encoded || !storage::PutLittleEndian(bytes, header_offset::CHECKSUM,
                                            ChecksumWithZeroedField(bytes, header_offset::CHECKSUM))) {
    return std::unexpected(Status::Corruption("internal WAL header layout exceeds its buffer"));
  }
  return output;
}

auto DecodeHeader(std::span<const std::byte> bytes) -> Result<Header> {
  if (bytes.size() != HEADER_BYTES) {
    return std::unexpected(Status::Corruption("WAL header has the wrong length"));
  }
  if (!std::ranges::equal(MAGIC, bytes.subspan(header_offset::MAGIC, MAGIC.size()))) {
    // As with superblocks, a foreign/old magic is a compatibility result;
    // damage to recognized current framing is corruption.
    return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB WAL magic"));
  }
  const auto checksum = storage::GetLittleEndian<std::uint32_t>(bytes, header_offset::CHECKSUM);
  if (!checksum || *checksum != ChecksumWithZeroedField(bytes, header_offset::CHECKSUM)) {
    return std::unexpected(Status::Corruption("WAL header checksum mismatch"));
  }
  const auto major = storage::GetLittleEndian<std::uint16_t>(bytes, header_offset::FORMAT_MAJOR);
  const auto minor = storage::GetLittleEndian<std::uint16_t>(bytes, header_offset::FORMAT_MINOR);
  const auto encoded_header_bytes = storage::GetLittleEndian<std::uint32_t>(bytes, header_offset::HEADER_BYTES);
  const auto required = storage::GetLittleEndian<std::uint64_t>(bytes, header_offset::REQUIRED_FEATURES);
  const auto optional = storage::GetLittleEndian<std::uint64_t>(bytes, header_offset::OPTIONAL_FEATURES);
  const auto segment_id = storage::GetLittleEndian<std::uint64_t>(bytes, header_offset::SEGMENT_ID);
  const auto starting_lsn = storage::GetLittleEndian<std::uint64_t>(bytes, header_offset::STARTING_LSN);
  if (!major || !minor || !encoded_header_bytes || !required || !optional || !segment_id || !starting_lsn) {
    return std::unexpected(Status::Corruption("truncated WAL header fields"));
  }
  if (*major != FORMAT_MAJOR || *minor > FORMAT_MINOR || *encoded_header_bytes != HEADER_BYTES ||
      (*required & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("unsupported TinyDB WAL version or features"));
  }
  auto result = Header{};
  if (!storage::GetBytes(bytes, header_offset::DATABASE_UUID, result.database_uuid)) {
    return std::unexpected(Status::Corruption("truncated WAL database UUID"));
  }
  result.segment_id = *segment_id;
  result.starting_lsn = *starting_lsn;
  result.required_features = *required;
  result.optional_features = *optional;
  if (!NonzeroUuid(result.database_uuid) || result.segment_id == 0 || result.starting_lsn == 0) {
    return std::unexpected(Status::Corruption("invalid WAL identity or starting LSN"));
  }
  if (std::ranges::any_of(bytes.subspan(header_offset::ENCODED_BYTES),
                          [](std::byte byte) { return byte != std::byte{0}; })) {
    // Nonzero reserved bytes may carry semantics in a future format and cannot
    // be ignored merely because the CRC is otherwise valid.
    return std::unexpected(Status::Corruption("nonzero reserved WAL header bytes"));
  }
  return result;
}

auto EncodeRecord(RecordType type, std::uint64_t transaction_id, std::uint64_t lsn, std::uint32_t record_sequence,
                  std::span<const std::byte> payload) -> Result<std::vector<char>> {
  auto output = std::vector<char>{};
  if (auto status = AppendRecord(output, type, transaction_id, lsn, record_sequence, payload); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return output;
}

auto DecodeRecord(std::span<const std::byte> bytes) -> Result<Record> {
  if (bytes.size() < RECORD_HEADER_BYTES) {
    return std::unexpected(Status::Corruption("truncated WAL record header"));
  }
  const auto total = storage::GetLittleEndian<std::uint32_t>(bytes, record_offset::TOTAL_BYTES);
  const auto raw_type = storage::GetLittleEndian<std::uint16_t>(bytes, record_offset::TYPE);
  const auto flags = storage::GetLittleEndian<std::uint16_t>(bytes, record_offset::FLAGS);
  const auto transaction_id = storage::GetLittleEndian<std::uint64_t>(bytes, record_offset::TRANSACTION_ID);
  const auto lsn = storage::GetLittleEndian<std::uint64_t>(bytes, record_offset::LSN);
  const auto record_sequence = storage::GetLittleEndian<std::uint32_t>(bytes, record_offset::RECORD_SEQUENCE);
  const auto checksum = storage::GetLittleEndian<std::uint32_t>(bytes, record_offset::CHECKSUM);
  const auto reserved = storage::GetLittleEndian<std::uint64_t>(bytes, record_offset::RESERVED);
  // Decode all fixed fields with checked helpers before any payload is
  // exposed. `bytes` must contain exactly one record, not a prefix followed by
  // ignored data.
  if (!total || !raw_type || !flags || !transaction_id || !lsn || !record_sequence || !checksum || !reserved ||
      *total != bytes.size() || *total < RECORD_HEADER_BYTES) {
    return std::unexpected(Status::Corruption("invalid WAL record length"));
  }
  if (*checksum != ChecksumWithZeroedField(bytes, record_offset::CHECKSUM)) {
    return std::unexpected(Status::Corruption("WAL record checksum mismatch"));
  }
  const auto type = static_cast<RecordType>(*raw_type);
  if (!KnownRecordType(type) || *flags != 0 || *reserved != 0 || *transaction_id == 0 || *lsn == 0) {
    // A valid CRC says the bytes arrived intact; it does not make impossible
    // metadata meaningful. Semantic validation remains mandatory.
    return std::unexpected(Status::Corruption("invalid WAL record metadata"));
  }
  return Record{.type = type,
                .transaction_id = *transaction_id,
                .lsn = *lsn,
                .record_sequence = *record_sequence,
                .payload = bytes.subspan(record_offset::PAYLOAD)};
}

auto EncodeTransaction(std::uint64_t transaction_id, std::uint64_t first_lsn, std::span<const PageImageView> pages,
                       txn::DatabaseState state) -> Result<EncodedTransaction> {
  if (transaction_id == 0 || first_lsn == 0 || pages.empty() ||
      pages.size() > std::numeric_limits<std::uint32_t>::max() - 2U) {
    return std::unexpected(Status::InvalidArgument("invalid WAL transaction identity or page count"));
  }

  /*
  ** LSNs advance once per record. The complete record count is known before
  ** encoding, so the commit LSN can be installed in DATABASE_STATE before any
  ** bytes are emitted.
  */
  const auto commit_lsn = first_lsn + pages.size() + 1U;
  if (commit_lsn < first_lsn || commit_lsn == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("WAL LSN space exhausted"));
  }
  state.transaction_id = transaction_id;
  state.visible_lsn = commit_lsn;
  const auto state_payload = EncodeDatabaseState(state);
  if (!state_payload) {
    return std::unexpected(state_payload.error());
  }

  auto output = std::vector<char>{};
  output.reserve(pages.size() * (RECORD_HEADER_BYTES + PAGE_IMAGE_PAYLOAD_BYTES) + RECORD_HEADER_BYTES +
                 DATABASE_STATE_PAYLOAD_BYTES + RECORD_HEADER_BYTES + COMMIT_PAYLOAD_BYTES);
  auto sequence = std::uint32_t{0};
  auto previous_page_id = HEADER_PAGE_ID;

  for (const auto &page : pages) {
    // Commit preparation supplies the transaction overlay's canonical page-ID
    // order. Checking adjacency proves both order and uniqueness without a
    // hash-table allocation on every commit.
    if (page.page_id < FIRST_DATA_PAGE_ID || page.page_id <= previous_page_id) {
      return std::unexpected(
          Status::InvalidArgument("WAL transaction contains unordered, invalid, or duplicate page IDs"));
    }
    previous_page_id = page.page_id;
    const auto decoded = storage::DecodeDataPageHeader(std::as_bytes(page.bytes), page.page_id);
    if (!decoded) {
      return std::unexpected(decoded.error());
    }
    if (auto status = AppendEncodedRecord(
            output, RecordType::PageImage, transaction_id, first_lsn + sequence, sequence, PAGE_IMAGE_PAYLOAD_BYTES,
            [&page](std::span<std::byte> payload) {
              return storage::PutLittleEndian(payload, PAGE_IMAGE_PAGE_ID_OFFSET, page.page_id) &&
                     storage::PutBytes(payload, PAGE_IMAGE_DATA_OFFSET, std::as_bytes(page.bytes));
            });
        !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    ++sequence;
  }

  if (auto status = AppendRecord(output, RecordType::DatabaseState, transaction_id, first_lsn + sequence, sequence,
                                 *state_payload);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  ++sequence;

  auto commit_payload = std::array<std::byte, COMMIT_PAYLOAD_BYTES>{};
  const auto encoded_commit =
      storage::PutLittleEndian(commit_payload, COMMIT_FIRST_LSN_OFFSET, first_lsn) &&
      storage::PutLittleEndian(commit_payload, COMMIT_FINAL_LSN_OFFSET, commit_lsn) &&
      storage::PutLittleEndian(commit_payload, COMMIT_PAGE_COUNT_OFFSET, static_cast<std::uint32_t>(pages.size())) &&
      storage::PutLittleEndian(commit_payload, COMMIT_RECORD_COUNT_OFFSET, sequence) &&
      storage::PutLittleEndian(commit_payload, COMMIT_TRANSACTION_DIGEST_OFFSET,
                               Crc32(std::as_bytes(std::span{output}))) &&
      storage::PutLittleEndian(commit_payload, COMMIT_STATE_DIGEST_OFFSET, Crc32(*state_payload));
  if (!encoded_commit) {
    return std::unexpected(Status::Corruption("internal WAL commit layout exceeds its buffer"));
  }
  if (auto status = AppendRecord(output, RecordType::Commit, transaction_id, commit_lsn, sequence, commit_payload);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  return EncodedTransaction{
      .transaction_id = transaction_id,
      .first_lsn = first_lsn,
      .commit_lsn = commit_lsn,
      .next_lsn = commit_lsn + 1U,
      .state = state,
      .bytes = std::move(output),
  };
}

auto DecodeTransaction(std::span<const std::byte> bytes,
                       std::uint64_t expected_first_lsn) -> Result<DecodedTransaction> {
  if (bytes.empty() || expected_first_lsn == 0) {
    return std::unexpected(Status::Corruption("empty WAL transaction"));
  }

  auto result = DecodedTransaction{
      .transaction_id = 0,
      .first_lsn = expected_first_lsn,
      .commit_lsn = 0,
      .next_lsn = 0,
      .state = {},
      .pages = {},
  };
  auto encoded_prefix_bytes = std::size_t{0};
  auto expected_sequence = std::uint32_t{0};
  auto offset = std::size_t{0};
  auto state_payload = std::span<const std::byte>{};
  auto previous_page_id = HEADER_PAGE_ID;

  while (offset < bytes.size()) {
    if (bytes.size() - offset < RECORD_HEADER_BYTES) {
      return std::unexpected(Status::Corruption("truncated WAL transaction record"));
    }
    const auto total = storage::GetLittleEndian<std::uint32_t>(bytes.subspan(offset), record_offset::TOTAL_BYTES);
    if (!total || *total < RECORD_HEADER_BYTES || *total > bytes.size() - offset) {
      return std::unexpected(Status::Corruption("invalid WAL transaction record length"));
    }
    const auto encoded_record = bytes.subspan(offset, *total);
    const auto record = DecodeRecord(encoded_record);
    if (!record) {
      return std::unexpected(record.error());
    }
    if (result.transaction_id == 0) {
      result.transaction_id = record->transaction_id;
    }
    if (record->transaction_id != result.transaction_id || record->record_sequence != expected_sequence ||
        record->lsn != expected_first_lsn + expected_sequence) {
      return std::unexpected(Status::Corruption("WAL transaction records are missing, duplicated, or reordered"));
    }

    if (record->type == RecordType::PageImage) {
      if (!state_payload.empty() || record->payload.size() != PAGE_IMAGE_PAYLOAD_BYTES) {
        return std::unexpected(Status::Corruption("WAL page image appears after database state"));
      }
      const auto page_id = storage::GetLittleEndian<page_id_t>(record->payload, PAGE_IMAGE_PAGE_ID_OFFSET);
      if (!page_id || *page_id < FIRST_DATA_PAGE_ID || *page_id <= previous_page_id) {
        return std::unexpected(Status::Corruption("unordered, invalid, or duplicate WAL page-image ID"));
      }
      previous_page_id = *page_id;
      auto page = DecodedPageImage{.page_id = *page_id, .bytes = {}};
      std::ranges::copy(std::span{record->payload}.subspan(PAGE_IMAGE_DATA_OFFSET),
                        reinterpret_cast<std::byte *>(page.bytes.data()));
      const auto decoded = storage::DecodeDataPageHeader(std::as_bytes(std::span{page.bytes}), *page_id);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      result.pages.push_back(page);
      encoded_prefix_bytes += encoded_record.size();
    } else if (record->type == RecordType::DatabaseState) {
      if (!state_payload.empty()) {
        return std::unexpected(Status::Corruption("WAL transaction contains duplicate database state"));
      }
      const auto decoded = DecodeDatabaseState(record->payload);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      result.state = *decoded;
      state_payload = record->payload;
      encoded_prefix_bytes += encoded_record.size();
    } else {
      if (state_payload.empty() || result.pages.empty() || record->payload.size() != COMMIT_PAYLOAD_BYTES ||
          offset + encoded_record.size() != bytes.size()) {
        return std::unexpected(Status::Corruption("WAL commit does not terminate one complete transaction"));
      }
      const auto first_lsn = storage::GetLittleEndian<std::uint64_t>(record->payload, COMMIT_FIRST_LSN_OFFSET);
      const auto final_lsn = storage::GetLittleEndian<std::uint64_t>(record->payload, COMMIT_FINAL_LSN_OFFSET);
      const auto page_count = storage::GetLittleEndian<std::uint32_t>(record->payload, COMMIT_PAGE_COUNT_OFFSET);
      const auto record_count = storage::GetLittleEndian<std::uint32_t>(record->payload, COMMIT_RECORD_COUNT_OFFSET);
      const auto transaction_digest =
          storage::GetLittleEndian<std::uint32_t>(record->payload, COMMIT_TRANSACTION_DIGEST_OFFSET);
      const auto state_digest = storage::GetLittleEndian<std::uint32_t>(record->payload, COMMIT_STATE_DIGEST_OFFSET);
      if (!first_lsn || !final_lsn || !page_count || !record_count || !transaction_digest || !state_digest ||
          *first_lsn != expected_first_lsn || *final_lsn != record->lsn || *page_count != result.pages.size() ||
          *record_count != expected_sequence || *transaction_digest != Crc32(bytes.first(encoded_prefix_bytes)) ||
          *state_digest != Crc32(state_payload) || result.state.transaction_id != result.transaction_id ||
          result.state.visible_lsn != record->lsn || record->lsn == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(Status::Corruption("WAL commit does not bind its complete transaction"));
      }
      result.commit_lsn = record->lsn;
      result.next_lsn = record->lsn + 1U;
      return result;
    }

    ++expected_sequence;
    offset += encoded_record.size();
  }
  return std::unexpected(Status::Corruption("WAL transaction has no commit record"));
}

}  // namespace tinydb::wal_format
