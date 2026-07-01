#pragma once

#include <tinydb/buffer_pool.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

// Largest key + value the tree accepts; Put aborts on anything bigger, so
// callers (the storage engine API) must reject oversized entries first. The
// cap guarantees any overflowing node has a valid split point (see the
// static_asserts in src/btree/node.h).
constexpr std::size_t MAX_ENTRY_BYTES = PAGE_SIZE / 4 - 32;

enum class NodeType : std::uint16_t {
  Leaf = 1,
  Internal = 2,
};

// Common node header for both leaf and internal nodes.
// Just for checking type for now
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
  std::uint8_t flags;
};

struct InternalCellHeader {
  page_id_t right_child;
  std::uint16_t key_size;
};

/**
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

/**
  B+ Tree with internal (nodes) pages and leaf (nodes) pages. Internal pages
  contain the header, then separator keys and child page ids so we can walk down
  the tree. The leaf nodes contain the actual data. A key points to a value, and
  the child pages at the bottom of the tree point to each other left to right,
  forming a sort of linked list.

  When we first create the B+ tree, we have just one page with id=root_page_id.
  At this point, it is both the root page and the single leaf page. Inserts add
  to that leaf until it fills. A full leaf splits into two leaves and copies the
  first key of the right leaf into the parent as a separator. If the parent is
  full, the internal split promotes its middle separator upward. This continues
  until some parent has room or the root itself splits.

*/
class BPlusTree {
 public:
  BPlusTree(BufferPool *buffer_pool, page_id_t root_page_id);

  void Put(std::string_view key, std::string_view value);
  auto Get(std::string_view key) -> std::optional<std::string>;
  void Remove(std::string_view key);
  auto Scan(std::string_view start, std::string_view end)
      -> std::vector<std::pair<std::string, std::string>>;

 private:
  BufferPool *buffer_pool_;
  page_id_t root_page_id_;
};
}  // namespace tinydb
