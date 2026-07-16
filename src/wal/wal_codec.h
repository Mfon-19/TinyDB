#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tinydb::wal_format {

// This magic intentionally differs from all prior WAL formats. There is no
// compatibility shim: opening old bytes as the new record framing could turn
// arbitrary data into apparently committed page images.
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

// Fixed 80-byte segment header. The unused tail stays zero and is included in
// the checksum, giving future versions extension space without changing where
// the first record starts.
//
//   0 magic[8]            32 database UUID[16]
//   8 major/minor         48 segment ID u64
//  12 header bytes u32    56 starting LSN u64
//  16 feature words       64 CRC-32 u32; 68..79 reserved
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

// Every record is self-framing. total_bytes permits forward scanning; the
// transaction ID groups page images with their commit; the LSN must equal the
// record's physical byte offset; and the CRC covers header plus payload.
//
//   0 total bytes u32     8 transaction ID u64   24 CRC-32 u32
//   4 type u16           16 LSN u64              28 reserved u32
//   6 flags u16                                  32 payload...
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

// Decoded records own their payload so recovery can validate and retain a
// complete transaction without borrowing from a reusable I/O buffer.
struct Record {
  RecordType type;
  std::uint64_t transaction_id;
  std::uint64_t lsn;
  std::vector<std::byte> payload;
};

// Required feature bits are a reader contract: any unknown bit rejects the
// file. Optional bits can be ignored by an older reader. Reserved bytes and
// flags must remain zero until a format version assigns them semantics.
auto EncodeHeader(const Header &header) -> Result<std::vector<char>>;
auto DecodeHeader(std::span<const std::byte> bytes) -> Result<Header>;
auto EncodeRecord(RecordType type, std::uint64_t transaction_id, std::uint64_t lsn,
                  std::span<const std::byte> payload) -> Result<std::vector<char>>;
auto DecodeRecord(std::span<const std::byte> bytes) -> Result<Record>;

}  // namespace tinydb::wal_format
