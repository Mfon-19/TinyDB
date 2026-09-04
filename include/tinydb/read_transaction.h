#pragma once

#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/detail/page_context.h"
#include <shared_mutex>

namespace tinydb {

class Database;

class ReadTransaction {
public:
  ReadTransaction(const ReadTransaction &) = delete;
  ReadTransaction &operator=(const ReadTransaction &) = delete;
  ReadTransaction(ReadTransaction &&) = delete;
  ReadTransaction &operator=(ReadTransaction &&) = delete;

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  auto Seek(std::string_view key) -> Result<btree::Cursor>;

private:
  friend class Database;
  explicit ReadTransaction(Database &database);

  std::shared_lock<std::shared_mutex> visibility_lock_;
  detail::PageContext context_;
  btree::BPlusTree tree_;
};

} // namespace tinydb
