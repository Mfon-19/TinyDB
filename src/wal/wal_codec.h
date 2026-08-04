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
** TinyDB WAL is a versioned, checksummed physical redo log. The file starts
** with one identity header and continues with self-framing transaction records.
**
**   low file offset
**          |
**          v
**   +--------------------------------------+
**   | WAL header (64 bytes)                |
**   +======================================+
**   | PageImage       commit LSN N         | \
**   +--------------------------------------+  |
**   | ... more PageImage records ...       |  | transaction N
**   +--------------------------------------+  |
**   | CommitState     commit LSN N         | /  terminal record
**   +======================================+
**   | Next transaction ...                 |
**   +--------------------------------------+
**          |
**          v
**   high file offset
**
** All records in one transaction carry the same commit LSN. PageImage records
** contain final physical pages. CommitState contains the resulting roots,
** logical page count, and number of PageImage records. Recovery accepts no
** page image until it validates the terminal CommitState record.
*/

inline constexpr auto MAGIC = std::array{
    std::byte{0x54}, std::byte{0x49}, std::byte{0x4E}, std::byte{0x59},
    std::byte{0x57}, std::byte{0x4C}, std::byte{0x30}, std::byte{0x36},
};  // "TINYWL06"
inline constexpr std::size_t HEADER_BYTES = 64;
inline constexpr std::size_t RECORD_HEADER_BYTES = 24;
inline constexpr std::uint16_t FORMAT_MAJOR = 1;
inline constexpr std::uint16_t FORMAT_MINOR = 0;
inline constexpr std::uint64_t SUPPORTED_REQUIRED_FEATURES = 0;

enum class RecordType : std::uint16_t {
  PageImage = 1,
  CommitState = 2,
};

/*
** Fixed 64-byte WAL header. The unused tail stays zero and is included in
** the checksum, leaving extension space without moving the first record.
**
**   0 magic[8]            32 database UUID[16]
**   8 major/minor         48 starting LSN u64
**  12 header bytes u32    56 CRC-32 u32; 60..63 reserved
**  16 feature words
*/
namespace header_offset {
inline constexpr std::size_t MAGIC = 0;
inline constexpr std::size_t FORMAT_MAJOR = 8;
inline constexpr std::size_t FORMAT_MINOR = 10;
inline constexpr std::size_t HEADER_BYTES = 12;
inline constexpr std::size_t REQUIRED_FEATURES = 16;
inline constexpr std::size_t OPTIONAL_FEATURES = 24;
inline constexpr std::size_t DATABASE_UUID = 32;
inline constexpr std::size_t STARTING_LSN = 48;
inline constexpr std::size_t CHECKSUM = 56;
inline constexpr std::size_t ENCODED_BYTES = 60;
}  // namespace header_offset

/*
** Every record is self-framing:
**
**   0 total bytes u32     8 commit LSN u64
**   4 type u16           16 CRC-32 u32
**   6 flags u16          20 reserved u32
**                        24 payload...
*/
namespace record_offset {
inline constexpr std::size_t TOTAL_BYTES = 0;
inline constexpr std::size_t TYPE = 4;
inline constexpr std::size_t FLAGS = 6;
inline constexpr std::size_t LSN = 8;
inline constexpr std::size_t CHECKSUM = 16;
inline constexpr std::size_t RESERVED = 20;
inline constexpr std::size_t PAYLOAD = RECORD_HEADER_BYTES;
}  // namespace record_offset

/*
** PAGE_IMAGE is exactly one final data page, never a superblock. The page's
** authenticated common header already contains its page ID and commit LSN.
** Therefore, the WAL does not duplicate either field.
**
** COMMIT_STATE closes the transaction. It contains metadata that is not in the
** page images:
**
**   0  B+ tree root page ID u64       16  logical page count u64
**   8  allocator root page ID u64     24  PageImage record count u32
**                                      28  reserved u32
*/
inline constexpr std::size_t PAGE_IMAGE_PAYLOAD_BYTES = PAGE_SIZE;

inline constexpr std::size_t COMMIT_STATE_ROOT_OFFSET = 0;
inline constexpr std::size_t COMMIT_STATE_ALLOCATOR_ROOT_OFFSET = 8;
inline constexpr std::size_t COMMIT_STATE_LOGICAL_PAGE_COUNT_OFFSET = 16;
inline constexpr std::size_t COMMIT_STATE_PAGE_IMAGE_COUNT_OFFSET = 24;
inline constexpr std::size_t COMMIT_STATE_RESERVED_OFFSET = 28;
inline constexpr std::size_t COMMIT_STATE_PAYLOAD_BYTES = 32;

// Header contains the variable identity and compatibility fields of the fixed
// WAL file header. The codec supplies its magic, version, size, and checksum.
struct Header {
  DatabaseUuid database_uuid{};
  std::uint64_t starting_lsn{1};
  std::uint64_t required_features{0};
  std::uint64_t optional_features{0};

  auto operator==(const Header &) const -> bool = default;
};

// Record is a validated, non-owning view of one encoded WAL record. Its payload
// refers to the input bytes and cannot outlive them.
struct Record {
  RecordType type;
  std::uint64_t lsn;
  std::span<const std::byte> payload;
};

// PageImageView is a non-owning encoder input for one final data-page image.
// validated_header can supply an existing validation result for the same bytes.
struct PageImageView {
  page_id_t page_id;
  std::span<const char, PAGE_SIZE> bytes;
  const storage::DataPageHeader *validated_header{nullptr};
};

// DecodedPageImage owns one validated data-page image from a WAL transaction.
struct DecodedPageImage {
  page_id_t page_id;
  std::array<char, PAGE_SIZE> bytes;
};

// EncodedTransaction owns the complete record sequence for one prepared WAL
// commit. The byte sequence does not include the WAL file header.
struct EncodedTransaction {
  std::uint64_t commit_lsn;
  std::vector<char> bytes;
};

// DecodedTransaction owns one complete and validated WAL transaction. It
// contains the resulting database state and the final page images.
struct DecodedTransaction {
  std::uint64_t commit_lsn;
  txn::DatabaseState state;
  std::vector<DecodedPageImage> pages;
};

auto EncodeHeader(const Header &header) -> Result<std::vector<char>>;
auto DecodeHeader(std::span<const std::byte> bytes) -> Result<Header>;
auto EncodeRecord(RecordType type, std::uint64_t lsn, std::span<const std::byte> payload) -> Result<std::vector<char>>;
auto DecodeRecord(std::span<const std::byte> bytes) -> Result<Record>;

/*
** Encode or decode exactly one complete transaction. Encode assigns the
** one commit LSN to every record. Decode rejects any missing, duplicated,
** reordered, corrupt, or trailing record.
*/
// Page images must be in strictly increasing page-ID order.
auto EncodeTransaction(std::uint64_t commit_lsn, std::span<const PageImageView> pages,
                       txn::DatabaseState state) -> Result<EncodedTransaction>;
auto DecodeTransaction(std::span<const std::byte> bytes,
                       std::uint64_t expected_commit_lsn) -> Result<DecodedTransaction>;

}  // namespace tinydb::wal_format
