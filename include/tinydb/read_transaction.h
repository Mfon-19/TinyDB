#pragma once

#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/detail/page_context.h"
#include <shared_mutex>

namespace tinydb {

class Database;

class ReadTransaction {
public:
  ReadTransaction(const ReadTransaction &) = delete;
  auto operator=(const ReadTransaction &) -> ReadTransaction & = delete;
  ReadTransaction(ReadTransaction &&) = delete;
  auto operator=(ReadTransaction &&) -> ReadTransaction & = delete;

  [[nodiscard]] auto Get(std::string_view key)
      -> Result<std::optional<std::string>>;
  [[nodiscard]] auto Seek(std::string_view key) -> Result<Cursor>;

private:
  friend class Database;
  explicit ReadTransaction(Database &database);

  std::shared_lock<std::shared_mutex> visibility_lock_;
  detail::PageContext context_;
  btree::BPlusTree tree_;
};

} // namespace tinydb
