#pragma once

/*
 * A B+ tree over frames in our buffer pool
 */

#include "tinydb/cache/buffer_pool.h"
#include "tinydb/storage/page.h"
#include "tinydb/storage/page_codec.h"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb::btree {

class BPlusTree {
public:
  BPlusTree(cache::BufferPool &buffer_pool,
            storage::PageId root_page_id) noexcept
      : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

  Status Initialize();
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  Status Put(std::string_view key, std::string_view value);
  auto Delete(std::string_view key) -> Result<bool>;
  Status RebuildFreeList();

private:
  struct Split {
    storage::PageId left;
    std::string separator;
    storage::PageId right;
  };

  auto FindLeaf(std::string_view key) -> Result<storage::PageId>;
  auto Insert(storage::PageId page_id, std::string_view key,
              std::string_view value) -> Result<std::optional<Split>>;
  auto Remove(storage::PageId page_id, std::string_view key,
              std::vector<storage::PageId> &freed_pages) -> Result<bool>;
  auto MergeChildren(storage::PageId left, storage::PageId right,
                     std::string_view separator) -> Result<bool>;
  Status CollapseRoot(storage::PageId child);
  auto SplitLeaf(storage::PageId page_id, storage::PageId next_leaf,
                 std::span<const storage::LeafEntry> entries) -> Result<Split>;
  auto SplitInternal(storage::PageId page_id, storage::PageId leftmost_child,
                     std::span<const storage::InternalEntry> entries)
      -> Result<Split>;
  auto AllocateSplit(storage::PageId page_id,
                     std::string_view separator) -> Result<Split>;
  auto WriteSplit(Split split, Result<storage::PageBytes> left,
                  Result<storage::PageBytes> right) -> Result<Split>;

  cache::BufferPool &buffer_pool_;
  storage::PageId root_page_id_;
};

} // namespace tinydb::btree
