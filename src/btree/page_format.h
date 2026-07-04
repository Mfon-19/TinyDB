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

// Value 3 is taken: FREE_PAGE_TYPE in file_header.h marks a page on the
// free list, which is never a tree node.
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

// Why splitting a node can never fail.
//
// These asserts pin the invariant that a record's footprint in a page (its
// slot plus its aligned cell, worst case shown below) never exceeds a
// quarter of a node's usable bytes (PAGE_SIZE minus the node header). That
// quarter bound is what guarantees ChooseSplitIndex always finds a split
// point where both halves fit in a page, so its "no valid split point"
// check is unreachable.
//
// The argument, with U = usable bytes and F = max footprint <= U/4:
//   1. Records shift between halves one at a time, so some split point is
//      off-balance by at most one record: bigger half <= (total + F) / 2.
//   2. The most bytes a split is ever asked to divide is an underflow
//      rebalance, which pools an underfull node (below U/2), a full
//      sibling (up to U), and one separator: total <= 1.5*U + F.
//      (An insert overflow is milder: U plus one new record.)
//   Combined: bigger half <= (1.5*U + 2*(U/4)) / 2 = U. It fits, exactly.
//
// MAX_ENTRY_BYTES caps key + value, so it bounds a leaf cell; a separator
// is a copy of a leaf key, so it bounds an internal cell too. If the
// balance ever shifts — larger MAX_ENTRY_BYTES, smaller PAGE_SIZE, fatter
// headers — these fire at compile time instead of ChooseSplitIndex
// aborting at runtime.
static_assert(SLOT_SIZE + AlignUp(sizeof(LeafCellHeader) + MAX_ENTRY_BYTES, alignof(LeafCellHeader)) <=
              (PAGE_SIZE - sizeof(LeafHeader)) / 4);
static_assert(SLOT_SIZE + AlignUp(sizeof(InternalCellHeader) + MAX_ENTRY_BYTES, alignof(InternalCellHeader)) <=
              (PAGE_SIZE - sizeof(InternalHeader)) / 4);

}  // namespace tinydb
