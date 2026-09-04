#pragma once

/*
 * A write transaction over a private context
 */

#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/detail/page_context.h"
#include <mutex>

namespace tinydb {

class Database;

class WriteTransaction {
public:
  WriteTransaction(const WriteTransaction &) = delete;
  WriteTransaction &operator=(const WriteTransaction &) = delete;
  WriteTransaction(WriteTransaction &&) = delete;
  WriteTransaction &operator=(WriteTransaction &&) = delete;

  auto Get(std::string_view key) -> Result<std::optional<std::string>>;
  Status Put(std::string_view key, std::string_view value);
  auto Delete(std::string_view key) -> Result<bool>;
  auto Seek(std::string_view key) -> Result<btree::Cursor>;
  Status Commit();

private:
  friend class Database;
  explicit WriteTransaction(Database &database);
  void Finish();

  Database &database_;
  std::unique_lock<std::mutex> writer_lock_;
  detail::WriteState state_;
  detail::PageContext context_;
  btree::BPlusTree tree_;
};

} // namespace tinydb
