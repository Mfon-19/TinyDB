#pragma once

/*
 * A B+ tree over a transaction page context
 */

#include "tinydb/cursor.h"
#include "tinydb/detail/page_context.h"
#include "tinydb/limits.h"
#include "tinydb/storage/page.h"
#include "tinydb/storage/page_codec.h"
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb::btree {

class BPlusTree {
public:
  BPlusTree(detail::PageContext &context, storage::PageId root_page_id) noexcept
      : context_(context), root_page_id_(root_page_id) {}

  auto Initialize() -> Status;
  [[nodiscard]] auto Get(std::string_view key)
      -> Result<std::optional<std::string>>;
  auto Put(std::string_view key, std::string_view value) -> Status;
  [[nodiscard]] auto Delete(std::string_view key) -> Result<bool>;
  [[nodiscard]] auto Seek(std::string_view key) -> Result<Cursor>;
  [[nodiscard]] auto FindFreePages(storage::PageId page_count)
      -> Result<std::vector<storage::PageId>>;

private:
  struct Split {
    storage::PageId left;
    std::string separator;
    storage::PageId right;
  };

  auto FindLeaf(std::string_view key) -> Result<storage::Page>;
  auto Insert(storage::PageId page_id, std::string_view key,
              std::string_view value) -> Result<std::optional<Split>>;
  auto Remove(storage::PageId page_id, std::string_view key) -> Result<bool>;
  auto MergeChildren(storage::PageId target, storage::PageId left,
                     storage::PageId right,
                     std::string_view separator) -> Result<bool>;
  auto SplitLeaf(storage::PageId page_id, storage::PageId next_leaf,
                 std::span<const storage::LeafEntry> entries) -> Result<Split>;
  auto SplitInternal(storage::PageId page_id, storage::PageId leftmost_child,
                     std::span<const storage::InternalEntry> entries)
      -> Result<Split>;
  auto AllocateSplit(storage::PageId page_id,
                     std::string_view separator) -> Result<Split>;
  auto WriteSplit(Split split, const storage::Page &left,
                  const storage::Page &right) -> Result<Split>;

  detail::PageContext &context_;
  const storage::PageId root_page_id_;
};

} // namespace tinydb::btree
