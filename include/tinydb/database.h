#pragma once

/*
 * This is the API to the database that callers use.
 */

#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/cache/buffer_pool.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tinydb {

class Database {
public:
  static Result<std::unique_ptr<Database>>
  Open(std::string_view name, std::size_t buffer_pool_capacity);

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Database(Database &&) = delete;
  Database &operator=(Database &&) = delete;

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  Status Put(std::string_view key, std::string_view value);
  auto Delete(std::string_view key) -> Result<bool>;
  auto Seek(std::string_view key) -> Result<btree::Cursor>;

private:
  Database(cache::BufferPool buffer_pool, storage::PageId root_page_id);

  cache::BufferPool buffer_pool_;
  btree::BPlusTree tree_;
};
} // namespace tinydb
