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
** Each encoded data page occupies exactly PAGE_SIZE bytes. Database pages 0
** and 1 are superblocks and use a different format.
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
** The type selects the payload decoder. Leaf and Internal payloads contain B+
** tree nodes. Allocator payloads contain free extents. Overflow payloads
** contain chunks of values stored across pages.
**
** A reader examines the size, magic, checksum, version, type, physical page
** ID, payload bounds, and flags before it reads payload fields. Persistent
** bytes are untrusted input. A decoder reports Corruption or UnsupportedFormat.
** It does not call TINYDB_CHECK for invalid persistent bytes.
**
** Page construction has three steps. InitializeDataPage fills all 4096 bytes
** with zero and writes the common header. A type-specific builder writes the
** payload. FinalizeDataPage stores the CRC-32. The checksum covers all 4096
** bytes and treats its field as zero. This order prevents stale data in unused
** space.
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
};

// A FreeExtent describes one consecutive range of page IDs that the latest
// committed state no longer uses. The range starts at first_page_id and
// contains page_count pages. The allocator can reuse the range only after the
// durable checkpoint LSN reaches retire_lsn. Before that point, the durable
// checkpoint can still refer to the previous contents of these pages.
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

void InitializeDataPage(std::span<std::byte, PAGE_SIZE> page, DataPageType type, page_id_t page_id,
                        std::uint64_t page_lsn, std::uint16_t payload_bytes);
void FinalizeDataPage(std::span<std::byte, PAGE_SIZE> page);

// Assigns the durable LSN and seals trusted transaction-private bytes. Common
// fields are checked, but the provisional checksum is deliberately not read.
// The returned header is the proof attached to the now-immutable page image,
// so WAL and cache preparation do not need to checksum the same bytes again.
auto RewriteDataPageLsn(std::span<std::byte, PAGE_SIZE> page, page_id_t expected_page_id,
                        std::uint64_t page_lsn) -> Result<DataPageHeader>;

/*
** expected_page_id is the physical file position. Comparing it with the
** persisted identity detects misplaced writes and stale bytes exposed by page
** reuse.
*/
auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader>;

inline constexpr std::size_t FREE_EXTENTS_PER_PAGE = 168;

// Type-specific initializers produce canonical private bytes with a zero
// checksum. Commit sealing or an explicit FinalizeDataPage supplies the
// persistent checksum.
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

// Decode canonical transaction-private bytes whose PageHandle carries an
// overflow payload proof. Common fields and overflow structure are checked,
// but the checksum is deferred until commit seals the final page image.
auto DecodePrivateOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage>;

}  // namespace tinydb::storage
