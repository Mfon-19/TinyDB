#pragma once

#include <tinydb/buffer_pool.h>
#include <tinydb/status.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

// Largest key + value the tree accepts; Put aborts on anything bigger, so
// callers (the storage engine API) must reject oversized entries first. The
// cap guarantees any overflowing node has a valid split point (see the
// static_asserts in src/btree/page_format.h).
constexpr std::size_t MAX_ENTRY_BYTES = PAGE_SIZE / 4 - 32;

/**
  B+ Tree with internal (nodes) pages and leaf (nodes) pages. Internal pages
  contain the header, then separator keys and child page ids so we can walk
  down the tree. The leaf nodes contain the actual data. A key points to a
  value, and the child pages at the bottom of the tree point to each other
  left to right, forming a sort of linked list.

  When we first create the B+ tree, we have just one page with
  id=root_page_id. At this point, it is both the root page and the single
  leaf page. Inserts add to that leaf until it fills. A full leaf splits into
  two leaves and copies the first key of the right leaf into the parent as a
  separator. If the parent is full, the internal split promotes its middle
  separator upward. This continues until some parent has room or the root
  itself splits.

  The on-disk page formats live in src/btree/page_format.h; the node codecs
  over them are the LeafNode and InternalNode classes in src/btree/.
*/
class BPlusTree {
 public:
  // Attaches to the tree rooted at root_page_id, bootstrapping a zeroed
  // page into an empty leaf. Fails if the root page cannot be read or does
  // not hold a tree node.
  static auto Open(BufferPool *buffer_pool, page_id_t root_page_id) -> Result<BPlusTree>;

  auto Put(std::string_view key, std::string_view value) -> Status;
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Remove(std::string_view key) -> Status;
  auto Scan(std::string_view start, std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>>;

 private:
  BPlusTree(BufferPool *buffer_pool, page_id_t root_page_id)
      : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

  BufferPool *buffer_pool_;
  page_id_t root_page_id_;
};
}  // namespace tinydb
