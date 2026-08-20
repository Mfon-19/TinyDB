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
** Each encoded data page occupies exactly PAGE_SIZE bytes; database pages 0
** and 1 are superblocks and use the separate format in superblock.h.
**
**   low byte offset
**          |
**          v
**   +--------------------------------------------------+ 0
**   | Common data-page header (32 bytes)               |
**   | magic, type, version, page ID, page LSN,         |
**   | payload_bytes, flags, and CRC-32                 |
**   +==================================================+ 32
**   | Type-specific payload                            |
**   | payload_bytes bytes                              |
**   +--------------------------------------------------+ 32 + payload_bytes
**   | Zero-filled unused tail (possibly empty)         |
**   +--------------------------------------------------+ PAGE_SIZE (4096)
**          |
**          v
**   high byte offset
**
** The type field selects the payload decoder: Leaf and Internal pages contain
** B+ tree nodes, Allocator pages contain free extents, and Overflow pages
** contain chunks of values that do not fit in a leaf record.
**
** Before interpreting a payload, a reader checks the page size, magic,
** checksum, version, type, physical page number, payload bounds, and flags;
** because persistent bytes are untrusted input, invalid bytes produce
** Corruption or UnsupportedFormat rather than an internal TINYDB_CHECK failure.
**
** Page construction begins with InitializeDataPage(), which zeros all 4096
** bytes and writes the common header, after which a type-specific builder
** writes the payload.  FinalizeDataPage() stores a CRC-32 over the complete
** page, with the checksum field treated as zero, so old data cannot survive in
** the unused tail of a newly encoded page.
*/
inline constexpr auto DATA_PAGE_MAGIC = std::array{
    std::byte{0x54},
    std::byte{0x44},
    std::byte{0x50},
    std::byte{0x35},
};  // "TDP5"
inline constexpr std::uint16_t DATA_PAGE_FORMAT_VERSION = 1;

enum class DataPageType : std::uint16_t {
  Leaf = 1,
  Internal = 2,
  Allocator = 3,
  Overflow = 4,
};

/*
** The common 32-byte data-page header has this layout:
**
**   0  magic[4]       8  page ID u64       24 payload bytes u16
**   4  type u16      16  page LSN u64      26 flags u16
**   6  version u16                         28 page CRC-32 u32
**
** The checksum covers all 4 KiB with its own field treated as zero, thereby
** protecting the header, live payload, reserved fields, and unused tail.
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
};

/*
** A FreeExtent describes a consecutive range of page numbers that the newest
** committed state no longer uses; the range begins at first_page_id and
** contains page_count pages.
**
** The allocator cannot reuse the range until the durable checkpoint LSN
** reaches retire_lsn, because before then the selected superblock can still
** lead recovery to the previous contents of these pages.
*/
struct FreeExtent {
  page_id_t first_page_id;
  std::uint64_t page_count;
  std::uint64_t retire_lsn;

  auto operator==(const FreeExtent &) const -> bool = default;
};

/*
** Allocator pages form a forward chain of sorted, non-overlapping, coalesced
** extents, with adjacent ranges represented by one extent so that the same
** free space has only one canonical encoding.
*/
struct FreeExtentPage {
  page_id_t next_page_id;
  std::vector<FreeExtent> extents;
};

/*
** OverflowPage is a borrowed view of one decoded overflow page.
** owner_value_id is the first page in the value chain, chunk_index starts at
** zero and increases without gaps, and payload refers into the encoded page;
** the caller must therefore keep those bytes alive while using the view.
*/
struct OverflowPage {
  std::uint64_t page_lsn;
  page_id_t owner_value_id;
  std::uint32_t chunk_index;
  page_id_t next_page_id;
  std::span<const std::byte> payload;
};

void InitializeDataPage(std::span<std::byte, PAGE_SIZE> page, DataPageType type, page_id_t page_id,
                        std::uint64_t page_lsn, std::uint16_t payload_bytes);
void FinalizeDataPage(std::span<std::byte, PAGE_SIZE> page);

/*
** Assign page_lsn to a trusted transaction-private page and seal its final
** checksum, checking the common fields but not the provisional checksum.  The
** returned header remains attached to the immutable page image as proof for
** WAL and cache preparation, avoiding another checksum pass.
*/
auto RewriteDataPageLsn(std::span<std::byte, PAGE_SIZE> page, page_id_t expected_page_id,
                        std::uint64_t page_lsn) -> Result<DataPageHeader>;

/*
** Decode and authenticate the common header of a persistent page.
** expected_page_id is the physical file position; comparing it with the stored
** page number detects misplaced writes and stale bytes exposed by page reuse.
*/
auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader>;

inline constexpr std::size_t FREE_EXTENTS_PER_PAGE = 168;

/*
** Type-specific initializers produce a canonical private page with a zero
** checksum; commit sealing, or an explicit FinalizeDataPage(), supplies the
** checksum before the page becomes persistent.
*/
auto InitializeFreeExtentPage(std::span<std::byte, PAGE_SIZE> page, page_id_t page_id, std::uint64_t page_lsn,
                              page_id_t next_page_id, std::span<const FreeExtent> extents) -> Status;
auto DecodeFreeExtentPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<FreeExtentPage>;
auto DecodeFreeExtentPage(std::span<const std::byte> page, page_id_t expected_page_id,
                          const DataPageHeader &validated_header) -> Result<FreeExtentPage>;

inline constexpr std::size_t OVERFLOW_PAGE_PAYLOAD_BYTES = PAGE_SIZE - 60;

auto InitializeOverflowPage(std::span<std::byte, PAGE_SIZE> page, page_id_t page_id, std::uint64_t page_lsn,
                            page_id_t owner_value_id, std::uint32_t chunk_index, page_id_t next_page_id,
                            std::span<const std::byte> payload) -> Status;
auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage>;
auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id,
                        const DataPageHeader &validated_header) -> Result<OverflowPage>;

/*
** Decode a canonical transaction-private overflow page whose PageHandle
** carries an overflow-payload proof.  This path checks the common fields and
** overflow structure but not the provisional checksum, which commit replaces
** after assigning the final LSN.
*/
auto DecodePrivateOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage>;

}  // namespace tinydb::storage
