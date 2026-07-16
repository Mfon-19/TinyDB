#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tinydb::wal_format {

inline constexpr auto MAGIC = std::array{
    std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59},
    std::byte{0x57}, std::byte{0x4C}, std::byte{0x30}, std::byte{0x34},
};  // "TINYWL04"
inline constexpr std::size_t HEADER_BYTES = 80;
inline constexpr std::size_t RECORD_HEADER_BYTES = 32;
inline constexpr std::uint16_t FORMAT_MAJOR = 1;
inline constexpr std::uint16_t FORMAT_MINOR = 0;
inline constexpr std::uint64_t SUPPORTED_REQUIRED_FEATURES = 0;

enum class RecordType : std::uint16_t {
  PageImage = 1,
  Commit = 2,
};

namespace header_offset {
inline constexpr std::size_t MAGIC = 0;
inline constexpr std::size_t FORMAT_MAJOR = 8;
inline constexpr std::size_t FORMAT_MINOR = 10;
inline constexpr std::size_t HEADER_BYTES = 12;
inline constexpr std::size_t REQUIRED_FEATURES = 16;
inline constexpr std::size_t OPTIONAL_FEATURES = 24;
inline constexpr std::size_t DATABASE_UUID = 32;
inline constexpr std::size_t SEGMENT_ID = 48;
inline constexpr std::size_t STARTING_LSN = 56;
inline constexpr std::size_t CHECKSUM = 64;
inline constexpr std::size_t ENCODED_BYTES = 68;
}  // namespace header_offset

namespace record_offset {
inline constexpr std::size_t TOTAL_BYTES = 0;
inline constexpr std::size_t TYPE = 4;
inline constexpr std::size_t FLAGS = 6;
inline constexpr std::size_t TRANSACTION_ID = 8;
inline constexpr std::size_t LSN = 16;
inline constexpr std::size_t CHECKSUM = 24;
inline constexpr std::size_t RESERVED = 28;
inline constexpr std::size_t PAYLOAD = RECORD_HEADER_BYTES;
}  // namespace record_offset

struct Header {
  DatabaseUuid database_uuid{};
  std::uint64_t segment_id{1};
  std::uint64_t starting_lsn{HEADER_BYTES};
  std::uint64_t required_features{0};
  std::uint64_t optional_features{0};

  auto operator==(const Header &) const -> bool = default;
};

struct Record {
  RecordType type;
  std::uint64_t transaction_id;
  std::uint64_t lsn;
  std::vector<std::byte> payload;
};

auto EncodeHeader(const Header &header) -> Result<std::vector<char>>;
auto DecodeHeader(std::span<const std::byte> bytes) -> Result<Header>;
auto EncodeRecord(RecordType type, std::uint64_t transaction_id, std::uint64_t lsn,
                  std::span<const std::byte> payload) -> Result<std::vector<char>>;
auto DecodeRecord(std::span<const std::byte> bytes) -> Result<Record>;

}  // namespace tinydb::wal_format
