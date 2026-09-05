#pragma once

#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/detail/page_context.h"
#include <mutex>

namespace tinydb {

class Database;

class WriteTransaction {
public:
  WriteTransaction(const WriteTransaction &) = delete;
  auto operator=(const WriteTransaction &) -> WriteTransaction & = delete;
  WriteTransaction(WriteTransaction &&) = delete;
  auto operator=(WriteTransaction &&) -> WriteTransaction & = delete;

  [[nodiscard]] auto Get(std::string_view key)
      -> Result<std::optional<std::string>>;
  auto Put(std::string_view key, std::string_view value) -> Status;
  [[nodiscard]] auto Delete(std::string_view key) -> Result<bool>;
  [[nodiscard]] auto Seek(std::string_view key) -> Result<Cursor>;
  auto Commit() -> Status;

private:
  friend class Database;
  explicit WriteTransaction(Database &database);

  Database &database_;
  std::unique_lock<std::mutex> writer_lock_;
  detail::WriteState state_;
  detail::PageContext context_;
  btree::BPlusTree tree_;
};

} // namespace tinydb
