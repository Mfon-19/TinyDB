#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tinydb::storage {

// Every non-superblock page starts with the same independently validated
// header. This lets the buffer pool reject misplaced, stale, or corrupted
// bytes before a page-type-specific decoder follows offsets inside them.
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

// Common 32-byte page header:
//
//   0  magic[4]       8  page ID u64       24 payload bytes u16
//   4  type u16      16  page LSN u64      26 flags u16
//   6  version u16                         28 page CRC-32 u32
//
// The checksum covers all 4 KiB with its own field treated as zero. Thus both
// meaningful fields and unused/reserved bytes are protected from corruption.
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

// A freed page becomes an allocator page whose payload is one link in the
// persistent LIFO free list. Page zero is the null/sentinel link.
struct AllocatorPage {
  page_id_t next_free;
};

// Large values will be split across overflow pages. total_value_bytes is
// repeated on the chain so readers can preflight allocation and validate that
// the chain reconstructs exactly the advertised logical value.
struct OverflowPage {
  std::uint64_t total_value_bytes;
  page_id_t next_page_id;
  std::vector<std::byte> payload;
};

// Page construction is deliberately two-phase. InitializeDataPage zeroes the
// page and writes the common header; the type-specific encoder then writes its
// payload; FinalizeDataPage seals the finished bytes with their checksum.
auto InitializeDataPage(std::span<std::byte> page, DataPageType type, page_id_t page_id, std::uint64_t page_lsn,
                        std::uint16_t payload_bytes) -> Status;
auto FinalizeDataPage(std::span<std::byte> page) -> Status;

// expected_page_id is the page's physical file position. Persisting the ID in
// the page and checking it here detects misplaced writes and stale page reuse.
auto DecodeDataPageHeader(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<DataPageHeader>;

auto EncodeAllocatorPage(page_id_t page_id, std::uint64_t page_lsn,
                         page_id_t next_free) -> Result<std::array<char, PAGE_SIZE>>;
auto DecodeAllocatorPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<AllocatorPage>;

auto EncodeOverflowPage(page_id_t page_id, std::uint64_t page_lsn, std::uint64_t total_value_bytes,
                        page_id_t next_page_id,
                        std::span<const std::byte> payload) -> Result<std::array<char, PAGE_SIZE>>;
auto DecodeOverflowPage(std::span<const std::byte> page, page_id_t expected_page_id) -> Result<OverflowPage>;

}  // namespace tinydb::storage
