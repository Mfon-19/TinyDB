#pragma once

#include <tinydb/bytes.h>
#include <tinydb/status.h>

#include "btree/value.h"
#include "storage/page_codec.h"
#include "txn/contract.h"

#include <cstddef>
#include <cstdint>

namespace tinydb {

/*
** B+ TREE PAGE FORMAT
**
** Tree pages use the common checksummed data-page header followed by a node
** header, an ascending slot directory, free space, and cells packed downward
** from the end of the page:
**
**   common header | node header | slots -> ... free ... <- encoded cells
**
** Slots appear in strict unsigned-byte key order. A leaf LINK is its forward
** successor or zero at end-of-chain. An internal LINK is its mandatory
** leftmost child; each separator cell stores the child on its right. Internal
** separators are inclusive lower bounds, so equality routes right.
**
** ValidateTreePage proves every offset, length, identity, link, reserved field,
** and key-order invariant before PageView exposes unchecked borrowed slices.
*/

/* Tree types are aliases of the common codec's persisted type values. */
enum class NodeType : std::uint16_t {
  Leaf = static_cast<std::uint16_t>(storage::DataPageType::Leaf),
  Internal = static_cast<std::uint16_t>(storage::DataPageType::Internal),
};

namespace node_page_offset {
inline constexpr std::size_t CELL_COUNT = storage::data_page_offset::HEADER_BYTES;
inline constexpr std::size_t FREE_START = CELL_COUNT + sizeof(std::uint16_t);
inline constexpr std::size_t FREE_END = FREE_START + sizeof(std::uint16_t);
inline constexpr std::size_t RESERVED = FREE_END + sizeof(std::uint16_t);
inline constexpr std::size_t LINK = RESERVED + sizeof(std::uint16_t);
inline constexpr std::size_t HEADER_BYTES = LINK + sizeof(page_id_t);
}  // namespace node_page_offset

namespace leaf_cell_offset {
/*
** Leaf cell, relative to its slot offset:
**
**   0 key bytes u16       5 key bytes
**   2 stored bytes u16    5+key stored value bytes
**   4 value kind u8
**
** Inline stored bytes are the value itself. Overflow stored bytes are the
** fixed 24-byte descriptor below. The logical value length is then the
** descriptor's u64, not the cell's u16 stored length.
*/
inline constexpr std::size_t KEY_BYTES = 0;
inline constexpr std::size_t VALUE_BYTES = 2;
inline constexpr std::size_t VALUE_KIND = 4;
inline constexpr std::size_t HEADER_BYTES = 5;
}  // namespace leaf_cell_offset

namespace overflow_descriptor_offset {
// 0 total u64 | 8 first/owner page u64 | 16 value CRC-32 | 20 reserved u32
inline constexpr std::size_t TOTAL_VALUE_BYTES = 0;
inline constexpr std::size_t FIRST_PAGE_ID = 8;
inline constexpr std::size_t VALUE_CHECKSUM = 16;
inline constexpr std::size_t RESERVED = 20;
}  // namespace overflow_descriptor_offset

namespace internal_cell_offset {
// Internal cell: [right child][key length][separator]. The first child is LINK.
inline constexpr std::size_t RIGHT_CHILD = 0;
inline constexpr std::size_t KEY_BYTES = 8;
inline constexpr std::size_t HEADER_BYTES = 10;
}  // namespace internal_cell_offset

using slot_t = std::uint16_t;
inline constexpr std::size_t SLOT_SIZE = sizeof(slot_t);
inline constexpr std::size_t LEAF_HEADER_SIZE = node_page_offset::HEADER_BYTES;
inline constexpr std::size_t INTERNAL_HEADER_SIZE = node_page_offset::HEADER_BYTES;
inline constexpr std::size_t LEAF_CELL_HEADER_SIZE = leaf_cell_offset::HEADER_BYTES;
inline constexpr std::size_t INTERNAL_CELL_HEADER_SIZE = internal_cell_offset::HEADER_BYTES;

auto RawNodeType(const char *page) -> std::uint16_t;

// Validates the common header plus all tree-local structural invariants.
auto ValidateTreePage(const char *page, page_id_t expected_page_id) -> Status;

// One record at or below half the usable bytes guarantees that every
// overflowing builder has a legal split boundary. Overflow descriptors keep a
// maximum-sized key below this bound regardless of logical value size.
inline constexpr std::size_t MAX_LEAF_RECORD_BYTES = (PAGE_SIZE - LEAF_HEADER_SIZE) / 2;
static_assert(SLOT_SIZE + LEAF_CELL_HEADER_SIZE + MAX_KEY_BYTES + OVERFLOW_VALUE_DESCRIPTOR_BYTES <=
              MAX_LEAF_RECORD_BYTES);
static_assert(SLOT_SIZE + INTERNAL_CELL_HEADER_SIZE + MAX_KEY_BYTES <= (PAGE_SIZE - INTERNAL_HEADER_SIZE) / 2);

}  // namespace tinydb
