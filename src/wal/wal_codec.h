#pragma once

#include <tinydb/status.h>
#include "storage/database_uuid.h"
#include "storage/page.h"
#include "storage/page_codec.h"

#include "txn/database_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tinydb::wal_format {

/*
** WAL RECORD FORMAT
**
** A WAL file begins with one checksummed identity header followed by
** self-framing records. PageImage records carry final physical data pages and
** DatabaseState carries the resulting roots and allocation frontier. A Commit
** record closes one contiguous run and binds its image count, order, encoded
** bytes, state, and commit LSN. Recovery accepts no part of the transaction
** until that closing record validates.
**
** Record LSNs form one global, monotonically increasing sequence independent
** of segment byte offsets. record_sequence starts at zero within each
** transaction. Together they detect splicing or reordering even when each
** individual record checksum remains valid.
** Required feature bits are a reader contract; flags and reserved bytes remain
** zero until a newer format assigns semantics to them.
**
** This magic intentionally differs from all prior WAL formats. There is no
** compatibility shim: interpreting old framing as current records could turn
** arbitrary bytes into apparently committed page images.
*/
inline constexpr auto MAGIC = std::array{
    std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59},
    std::byte{0x57}, std::byte{0x4C}, std::byte{0x30}, std::byte{0x35},
};  // "TINYWL05"
inline constexpr std::size_t HEADER_BYTES = 80;
inline constexpr std::size_t RECORD_HEADER_BYTES = 40;
inline constexpr std::uint16_t FORMAT_MAJOR = 1;
inline constexpr std::uint16_t FORMAT_MINOR = 0;
inline constexpr std::uint64_t SUPPORTED_REQUIRED_FEATURES = 0;

enum class RecordType : std::uint16_t {
  PageImage = 1,
  DatabaseState = 2,
  Commit = 3,
};

/*
** Fixed 80-byte segment header. The unused tail stays zero and is included in
** the checksum, leaving extension space without moving the first record.
**
**   0 magic[8]            32 database UUID[16]
**   8 major/minor         48 segment ID u64
**  12 header bytes u32    56 starting LSN u64
**  16 feature words       64 CRC-32 u32; 68..79 reserved
*/
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

/*
** Every record is self-framing:
**
**   0 total bytes u32     8 transaction ID u64   24 record sequence u32
**   4 type u16           16 LSN u64              28 CRC-32 u32
**   6 flags u16                                  32 reserved u64
**                                                40 payload...
*/
namespace record_offset {
inline constexpr std::size_t TOTAL_BYTES = 0;
inline constexpr std::size_t TYPE = 4;
inline constexpr std::size_t FLAGS = 6;
inline constexpr std::size_t TRANSACTION_ID = 8;
inline constexpr std::size_t LSN = 16;
inline constexpr std::size_t RECORD_SEQUENCE = 24;
inline constexpr std::size_t CHECKSUM = 28;
inline constexpr std::size_t RESERVED = 32;
inline constexpr std::size_t PAYLOAD = RECORD_HEADER_BYTES;
}  // namespace record_offset

/*
** Transaction payload geometry. PAGE_IMAGE records contain final data pages,
** never superblocks. DATABASE_STATE replaces logged superblock images and
** carries the roots/frontiers recovery needs to construct the next durable
** superblock. COMMIT authenticates all preceding records in the transaction.
*/
inline constexpr std::size_t PAGE_IMAGE_PAGE_ID_OFFSET = 0;
inline constexpr std::size_t PAGE_IMAGE_DATA_OFFSET = sizeof(page_id_t);
inline constexpr std::size_t PAGE_IMAGE_PAYLOAD_BYTES = PAGE_IMAGE_DATA_OFFSET + PAGE_SIZE;

inline constexpr std::size_t DATABASE_STATE_ROOT_OFFSET = 0;
inline constexpr std::size_t DATABASE_STATE_ALLOCATOR_ROOT_OFFSET = 8;
inline constexpr std::size_t DATABASE_STATE_HIGH_WATER_OFFSET = 16;
inline constexpr std::size_t DATABASE_STATE_TRANSACTION_ID_OFFSET = 24;
inline constexpr std::size_t DATABASE_STATE_VISIBLE_LSN_OFFSET = 32;
inline constexpr std::size_t DATABASE_STATE_CHECKPOINT_LSN_OFFSET = 40;
inline constexpr std::size_t DATABASE_STATE_PAYLOAD_BYTES = 48;

inline constexpr std::size_t COMMIT_FIRST_LSN_OFFSET = 0;
inline constexpr std::size_t COMMIT_FINAL_LSN_OFFSET = 8;
inline constexpr std::size_t COMMIT_PAGE_COUNT_OFFSET = 16;
inline constexpr std::size_t COMMIT_RECORD_COUNT_OFFSET = 20;
inline constexpr std::size_t COMMIT_TRANSACTION_DIGEST_OFFSET = 24;
inline constexpr std::size_t COMMIT_STATE_DIGEST_OFFSET = 28;
inline constexpr std::size_t COMMIT_PAYLOAD_BYTES = 32;

struct Header {
  DatabaseUuid database_uuid{};
  std::uint64_t segment_id{1};
  std::uint64_t starting_lsn{1};
  std::uint64_t required_features{0};
  std::uint64_t optional_features{0};

  auto operator==(const Header &) const -> bool = default;
};

// A decoded record is a validated view. Its payload borrows the exact encoded
// record supplied to DecodeRecord and cannot outlive those bytes.
struct Record {
  RecordType type;
  std::uint64_t transaction_id;
  std::uint64_t lsn;
  std::uint32_t record_sequence;
  std::span<const std::byte> payload;
};

struct PageImageView {
  page_id_t page_id;
  std::span<const char, PAGE_SIZE> bytes;
  const storage::DataPageHeader *validated_header{nullptr};
};

struct DecodedPageImage {
  page_id_t page_id;
  std::array<char, PAGE_SIZE> bytes;
};

struct EncodedTransaction {
  std::uint64_t transaction_id;
  std::uint64_t first_lsn;
  std::uint64_t commit_lsn;
  std::uint64_t next_lsn;
  txn::DatabaseState state;
  std::vector<char> bytes;
};

struct DecodedTransaction {
  std::uint64_t transaction_id;
  std::uint64_t first_lsn;
  std::uint64_t commit_lsn;
  std::uint64_t next_lsn;
  txn::DatabaseState state;
  std::vector<DecodedPageImage> pages;
};

// Required feature bits are a reader contract: any unknown bit rejects the
// file. Optional bits can be ignored by an older reader. Reserved bytes and
// flags must remain zero until a format version assigns them semantics.
auto EncodeHeader(const Header &header) -> Result<std::vector<char>>;
auto DecodeHeader(std::span<const std::byte> bytes) -> Result<Header>;
auto EncodeRecord(RecordType type, std::uint64_t transaction_id, std::uint64_t lsn, std::uint32_t record_sequence,
                  std::span<const std::byte> payload) -> Result<std::vector<char>>;
auto DecodeRecord(std::span<const std::byte> bytes) -> Result<Record>;

/*
** Encode or decode exactly one complete transaction. Encode assigns the
** transaction's visible LSN to its commit record. Decode rejects any missing,
** duplicated, reordered, corrupt, or trailing record.
*/
// Page images must be in strictly increasing page-ID order.
auto EncodeTransaction(std::uint64_t transaction_id, std::uint64_t first_lsn, std::span<const PageImageView> pages,
                       txn::DatabaseState state) -> Result<EncodedTransaction>;
auto DecodeTransaction(std::span<const std::byte> bytes,
                       std::uint64_t expected_first_lsn) -> Result<DecodedTransaction>;

}  // namespace tinydb::wal_format
