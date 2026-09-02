#pragma once

/*
 * A B+ tree over frames in our buffer pool
 */

#include "tinydb/cache/buffer_pool.h"
#include "tinydb/storage/page.h"
#include <optional>
#include <string>
#include <string_view>

namespace tinydb::btree {

class BPlusTree {
public:
  BPlusTree(cache::BufferPool &buffer_pool,
            storage::PageId root_page_id) noexcept
      : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

  Status Initialize();
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  Status Put(std::string_view key, std::string_view value);

private:
  cache::BufferPool &buffer_pool_;
  storage::PageId root_page_id_;
};

} // namespace tinydb::btree
