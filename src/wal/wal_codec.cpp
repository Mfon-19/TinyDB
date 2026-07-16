#include "wal/wal_codec.h"

#include "storage/encoding.h"
#include "util/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace tinydb::wal_format {
namespace {

auto NonzeroUuid(const DatabaseUuid &uuid) -> bool {
  return std::ranges::any_of(uuid, [](std::byte byte) { return byte != std::byte{0}; });
}

auto ChecksumWithZeroedField(std::span<const std::byte> input, std::size_t checksum_offset) -> std::uint32_t {
  auto copy = std::vector<std::byte>(input.begin(), input.end());
  std::ranges::fill(copy.begin() + static_cast<std::ptrdiff_t>(checksum_offset),
                    copy.begin() + static_cast<std::ptrdiff_t>(checksum_offset + sizeof(std::uint32_t)), std::byte{0});
  return Crc32(copy);
}

auto KnownRecordType(RecordType type) -> bool { return type == RecordType::PageImage || type == RecordType::Commit; }

}  // namespace

auto EncodeHeader(const Header &header) -> Result<std::vector<char>> {
  if (!NonzeroUuid(header.database_uuid) || header.segment_id == 0 || header.starting_lsn < HEADER_BYTES) {
    return std::unexpected(Status::InvalidArgument("invalid WAL header identity or LSN"));
  }
  if ((header.required_features & ~SUPPORTED_REQUIRED_FEATURES) != 0) {
    return std::unexpected(Status::UnsupportedFormat("unsupported required WAL feature"));
  }

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
  if (!NonzeroUuid(result.database_uuid) || result.segment_id == 0 || result.starting_lsn < HEADER_BYTES) {
    return std::unexpected(Status::Corruption("invalid WAL identity or starting LSN"));
  }
  if (std::ranges::any_of(bytes.subspan(header_offset::ENCODED_BYTES),
                          [](std::byte byte) { return byte != std::byte{0}; })) {
    return std::unexpected(Status::Corruption("nonzero reserved WAL header bytes"));
  }
  return result;
}

auto EncodeRecord(RecordType type, std::uint64_t transaction_id, std::uint64_t lsn,
                  std::span<const std::byte> payload) -> Result<std::vector<char>> {
  if (!KnownRecordType(type) || transaction_id == 0 || lsn < HEADER_BYTES ||
      payload.size() > std::numeric_limits<std::uint32_t>::max() - RECORD_HEADER_BYTES) {
    return std::unexpected(Status::InvalidArgument("invalid WAL record metadata"));
  }
  const auto total_bytes = RECORD_HEADER_BYTES + payload.size();
  auto output = std::vector<char>(total_bytes, 0);
  auto bytes = std::as_writable_bytes(std::span{output});
  const auto encoded =
      storage::PutLittleEndian(bytes, record_offset::TOTAL_BYTES, static_cast<std::uint32_t>(total_bytes)) &&
      storage::PutLittleEndian(bytes, record_offset::TYPE, static_cast<std::uint16_t>(type)) &&
      storage::PutLittleEndian(bytes, record_offset::FLAGS, std::uint16_t{0}) &&
      storage::PutLittleEndian(bytes, record_offset::TRANSACTION_ID, transaction_id) &&
      storage::PutLittleEndian(bytes, record_offset::LSN, lsn) &&
      storage::PutLittleEndian(bytes, record_offset::CHECKSUM, std::uint32_t{0}) &&
      storage::PutLittleEndian(bytes, record_offset::RESERVED, std::uint32_t{0}) &&
      storage::PutBytes(bytes, record_offset::PAYLOAD, payload);
  if (!encoded || !storage::PutLittleEndian(bytes, record_offset::CHECKSUM,
                                            ChecksumWithZeroedField(bytes, record_offset::CHECKSUM))) {
    return std::unexpected(Status::Corruption("internal WAL record layout exceeds its buffer"));
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
  const auto checksum = storage::GetLittleEndian<std::uint32_t>(bytes, record_offset::CHECKSUM);
  const auto reserved = storage::GetLittleEndian<std::uint32_t>(bytes, record_offset::RESERVED);
  if (!total || !raw_type || !flags || !transaction_id || !lsn || !checksum || !reserved || *total != bytes.size() ||
      *total < RECORD_HEADER_BYTES) {
    return std::unexpected(Status::Corruption("invalid WAL record length"));
  }
  if (*checksum != ChecksumWithZeroedField(bytes, record_offset::CHECKSUM)) {
    return std::unexpected(Status::Corruption("WAL record checksum mismatch"));
  }
  const auto type = static_cast<RecordType>(*raw_type);
  if (!KnownRecordType(type) || *flags != 0 || *reserved != 0 || *transaction_id == 0 || *lsn < HEADER_BYTES) {
    return std::unexpected(Status::Corruption("invalid WAL record metadata"));
  }
  return Record{.type = type,
                .transaction_id = *transaction_id,
                .lsn = *lsn,
                .payload = std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(record_offset::PAYLOAD),
                                                  bytes.end())};
}

}  // namespace tinydb::wal_format
