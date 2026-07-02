#pragma once

#include <tinydb/b_plus_tree.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

/**
  On-disk page formats for the B+ tree, shared by the two node codecs
  (leaf_node.cpp and internal_node.cpp). This header owns the raw structs;
  the tree algorithms never touch page bytes directly.

  Here is what a leaf page looks like, slotted page format:
  The slot array grows downward, the cells grow upward

  page 1
  ┌────────────────────────────────────────┐
  │   LeafHeader                           │
  │       type = leaf                      │
  │       cell_count = 4                   │
  │       free_start = after slot array    │
  │       free_end = before cell bytes     │
  ├────────────────────────────────────────┤
  │   slots, sorted by key                 │
  │       slot 0 -> offset of cell for "a" │
  │       slot 1 -> offset of cell for "b" │
  │       slot 2 -> offset of cell for "m" │
  │       slot 3 -> offset of cell for "z" │
  ├────────────────────────────────────────┤
  │   free space                           │
  ├────────────────────────────────────────┤
  │   cell bytes                           │
  │       key="z", value="..."             │
  │       key="m", value="..."             │
  │       key="b", value="..."             │
  │       key="a", value="..."             │
  └────────────────────────────────────────┘

  Cell bytes can be interpreted like so:
  cell + 0    cell + 2    cell + 4    cell + 5    key_bytes + key_size
  ┌──────────┬────────────┬──────────┬───────────┬─────────────┐
  │ key_size │  value_size│ flags    │ key bytes │  value bytes│
  │ uint16_t │  uint16_t  │ uint8_t  │    ...    │      ...    │
  └──────────┴────────────┴──────────┴───────────┴─────────────┘

  Internal page design. Separator key, same as leaf node key, child page id
  gives the page id to search downwards to, could be an internal or child
  page. An internal node with N keys has N + 1 children. Key size is variable
  bytes

  Keys:        K0, K1, K2
  Children:  C0, C1, C2, C3

  Here, C0 is the first_child. An internal cell is something like:
  K0 -> C1. C1 is the right_child

  To search with a given key,
  key < K0        -> C0
  K0 <= key < K1  -> C1
  K1 <= key < K2  -> C2
  key >= K2       -> C3

  internal page
  ┌────────────────────────────────────┐
  │ InternalHeader                     │
  │   type = internal                  │
  │   cell_count = 3                   │
  │   free_start                       │
  │   free_end                         │
  │   first_child = C0                 │
  ├────────────────────────────────────┤
  │ slots sorted by separator key      │
  │   slot 0 -> cell for K0 + C1       │
  │   slot 1 -> cell for K1 + C2       │
  │   slot 2 -> cell for K2 + C3       │
  ├────────────────────────────────────┤
  │ free space                         │
  ├────────────────────────────────────┤
  │ internal cells                     │
  │   right_child=C3, key=K2           │
  │   right_child=C2, key=K1           │
  │   right_child=C1, key=K0           │
  └────────────────────────────────────┘
*/

namespace tinydb {

enum class NodeType : std::uint16_t {
  Leaf = 1,
  Internal = 2,
};

// Common prefix of both node headers, for sniffing a page's type.
struct NodeHeader {
  NodeType type;
};

struct LeafHeader {
  NodeType type;
  std::uint16_t cell_count;  // Number of slots
  std::uint16_t free_start;  // Where the next slot should go
  std::uint16_t free_end;    // Where the next cell should go
  page_id_t next_leaf;
};

struct InternalHeader {
  NodeType type;
  std::uint16_t cell_count;  // Number of slots
  std::uint16_t free_start;  // Where the next slot should go
  std::uint16_t free_end;    // Where the next cell should go
  page_id_t first_child;
};

struct LeafCellHeader {
  std::uint16_t key_size;
  std::uint16_t value_size;
  std::uint8_t flags;  // non-zero marks a tombstone: skipped on load and
                       // dropped on the next rewrite
};

struct InternalCellHeader {
  page_id_t right_child;
  std::uint16_t key_size;
};

// Type-safe memcpy in and out of page bytes: page data is never accessed
// through pointer casts, so alignment never matters.
template <typename T>
auto ReadAs(const char *src) -> T {
  static_assert(std::is_trivially_copyable_v<T>);
  T value;
  std::memcpy(&value, src, sizeof(T));
  return value;
}

template <typename T>
void WriteAs(char *dst, const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(dst, &value, sizeof(T));
}

constexpr auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

constexpr auto AlignDown(std::size_t value, std::size_t alignment) -> std::size_t { return value & ~(alignment - 1); }

using slot_t = std::uint16_t;
constexpr std::size_t SLOT_SIZE = sizeof(slot_t);

// The quarter-page footprint invariant behind split-always-succeeds: with
// every record footprint at most a quarter of a node's usable bytes, the
// split with the smallest byte imbalance is off-balance by at most one
// footprint, so the bigger half stays within (total + usable/4) / 2 <= usable
// for every overflow the tree can build (an insert overflows one page by at
// most one record; underflow repair combines at most half a page, a full
// page, and one separator). A separator key is a leaf key, so
// MAX_ENTRY_BYTES bounds it.
static_assert(SLOT_SIZE + AlignUp(sizeof(LeafCellHeader) + MAX_ENTRY_BYTES, alignof(LeafCellHeader)) <=
              (PAGE_SIZE - sizeof(LeafHeader)) / 4);
static_assert(SLOT_SIZE + AlignUp(sizeof(InternalCellHeader) + MAX_ENTRY_BYTES, alignof(InternalCellHeader)) <=
              (PAGE_SIZE - sizeof(InternalHeader)) / 4);

}  // namespace tinydb
