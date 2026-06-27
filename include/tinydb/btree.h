#include <tinydb/buffer_pool.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

struct LeafHeader {
  std::uint16_t type;
  std::uint16_t cell_count;  // Number of slots
  std::uint16_t free_start;  // Where the next slot should go
  std::uint16_t free_end;    // Where the next cell should go
};

struct CellHeader {
  std::uint16_t key_size;
  std::uint16_t value_size;
  std::uint8_t flags;
};

/**
    Here is what a page looks like, slotted page format:
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
*/

class BTree {
 public:
  BTree(BufferPool *buffer_pool, page_id_t root_page_id);

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