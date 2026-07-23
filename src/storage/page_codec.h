#pragma once

#include <tinydb/status.h>
#include "storage/page.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tinydb::storage {

/*
** DATA PAGE FORMAT
**
** Every non-superblock page starts with one independently validated header.
** A reader validates size, magic, checksum, format, type, physical page ID,
** payload bounds, and flags before any type-specific decoder follows offsets
** inside the payload. Persistent bytes are untrusted input; decode failures
** return Corruption or UnsupportedFormat and never invoke TINYDB_CHECK.
**
** Page construction has two phases. InitializeDataPage zeroes the complete
** page and writes the common fields. A type-specific builder writes its
** payload. FinalizeDataPage then seals all 4096 bytes with a checksum. This
** ordering prevents deleted records, allocator fragments, or stale memory from
** surviving in checksum-covered free space.
*/
inline constexpr auto DATA_PAGE_MAGIC = std::array{
    std::byte{0x54},
    std::byte{0x44},
    std::byte{0x50},
    std::byte{0x34},
};  // "TDP4"
inline constexpr std::uint16_t DATA_PAGE_FORMAT_VERSION = 1;

enum class DataPageType : std::uint16_t {
  Leaf = 1,
  Internal = 2,
  Allocator = 3,
  Overflow = 4,
};

/*
** Common 32-byte page header:
**
**   0  magic[4]       8  page ID u64       24 payload bytes u16
**   4  type u16      16  page LSN u64      26 flags u16
**   6  version u16                         28 page CRC-32 u32
**
** The checksum covers all 4 KiB with its own field treated as zero. Both
** meaningful fields and unused or reserved bytes are therefore protected.
*/
namespace data_page_offset {
inline constexpr std::size_t MAGIC = 0;
inline constexpr std::size_t TYPE = 4;
inline constexpr std::size_t FORMAT_VERSION = 6;
inline constexpr std::size_t PAGE_ID = 8;
inline constexpr std::size_t PAGE_LSN = 16;
inline constexpr std::size_t PAYLOAD_BYTES = 24;
inline constexpr std::size_t FLAGS = 26;
inline constexpr std::size_t CHECKSUM = 28;
inline constexpr std::size_t HEADER_BYTES = 32;
}  // namespace data_page_offset

struct DataPageHeader {
  DataPageType type;
  page_id_t page_id;
  std::uint64_t page_lsn;
  std::uint16_t payload_bytes;
  std::uint16_t flags;
};

struct FreeExtent {
  page_id_t first_page_id;
  std::uint64_t page_count;
  std::uint64_t retire_lsn;

  auto operator==(const FreeExtent &) const -> bool = default;
};

/*
** Allocator pages form a forward chain of sorted, non-overlapping, coalesced
** extents. retire_lsn prevents reuse while an older checkpoint or WAL history
** can still associate the page ID with its previous contents.
*/
struct FreeExtentPage {
  page_id_t next_page_id;
  std::vector<FreeExtent> extents;
};

/*
** Borrowed view of one overflow page. owner_value_id is the chain's first page
** and chunk_index must increase from zero without gaps. The caller keeps the
** encoded page bytes alive while using payload.
*/
struct OverflowPage {
  std::uint64_t page_lsn;
  page_id_t owner_value_id;
  std::uint32_t chunk_index;
  page_id_t next_page_id;
  std::span<const std::byte> payload;
};

auto InitializeDataPage(std::span<std::byte> page, DataPageType type, page_id_t page_id, std::uint64_t page_lsn,
                        std::uint16_t payload_bytes) -> Status;
auto FinalizeDataPage(std::span<std::byte> page) -> Status;

// Assigns the durable LSN and seals trusted transaction-private bytes. Common
// fields are checked, but the provisional checksum is deliberately not read.
auto RewriteDataPageLsn(std::span<std::byte> page, page_id_t expected_page_id, std::uint64_t page_lsn) -> Status;

/*
** expected_page_id is the physical file position. Comparing it with the
** persisted identity detects misplaced writes and stale bytes exposed by page
** reuse.
*/
auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader>;

inline constexpr std::size_t FREE_EXTENTS_PER_PAGE = 168;

// Type-specific initializers produce canonical private bytes with a zero
// checksum. Persistent callers use the Encode wrappers below, which finalize
// the page before returning it.
auto InitializeFreeExtentPage(std::span<std::byte> page, page_id_t page_id, std::uint64_t page_lsn,
                              page_id_t next_page_id, std::span<const FreeExtent> extents) -> Status;
auto EncodeFreeExtentPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t next_page_id,
                          std::span<const FreeExtent> extents) -> Result<std::array<char, PAGE_SIZE>>;
auto DecodeFreeExtentPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<FreeExtentPage>;
auto DecodeFreeExtentPage(std::span<const std::byte> page, page_id_t expected_page_id,
                          const DataPageHeader &validated_header) -> Result<FreeExtentPage>;

inline constexpr std::size_t OVERFLOW_PAGE_PAYLOAD_BYTES = PAGE_SIZE - 60;

auto InitializeOverflowPage(std::span<std::byte> page, page_id_t page_id, std::uint64_t page_lsn,
                            page_id_t owner_value_id, std::uint32_t chunk_index, page_id_t next_page_id,
                            std::span<const std::byte> payload) -> Status;
auto EncodeOverflowPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t owner_value_id, std::uint32_t chunk_index,
                        page_id_t next_page_id,
                        std::span<const std::byte> payload) -> Result<std::array<char, PAGE_SIZE>>;
auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage>;
auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id,
                        const DataPageHeader &validated_header) -> Result<OverflowPage>;

}  // namespace tinydb::storage
