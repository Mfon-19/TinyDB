#include "tinydb/read_transaction.h"
#include "tinydb/database.h"

namespace tinydb {

ReadTransaction::ReadTransaction(Database &database)
    : visibility_lock_(database.visibility_mutex_),
      context_(database.buffer_pool_, database.poisoned_),
      tree_(context_, database.root_page_id_) {}

auto ReadTransaction::Get(std::string_view key)
    -> Result<std::optional<std::string>> {
  return tree_.Get(key);
}

auto ReadTransaction::Seek(std::string_view key) -> Result<btree::Cursor> {
  return tree_.Seek(key);
}

} // namespace tinydb
