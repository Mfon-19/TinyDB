#include "tinydb/write_transaction.h"
#include "tinydb/database.h"
#include <utility>

namespace tinydb {

WriteTransaction::WriteTransaction(Database &database)
    : database_(database), writer_lock_(database.writer_mutex_),
      state_{database.page_count_, database.free_pages_},
      context_(database.buffer_pool_, database.poisoned_, &state_),
      tree_(context_, database.root_page_id_) {}

auto WriteTransaction::Get(std::string_view key)
    -> Result<std::optional<std::string>> {
  auto result = tree_.Get(key);
  if (!result) {
    return Err(context_.Fail(std::move(result.error())));
  }
  return result;
}

auto WriteTransaction::Put(std::string_view key,
                           std::string_view value) -> Status {
  auto status = tree_.Put(key, value);
  if (!status.Ok()) {
    return context_.Fail(std::move(status));
  }
  return status;
}

auto WriteTransaction::Delete(std::string_view key) -> Result<bool> {
  auto result = tree_.Delete(key);
  if (!result) {
    return Err(context_.Fail(std::move(result.error())));
  }
  return result;
}

auto WriteTransaction::Seek(std::string_view key) -> Result<Cursor> {
  auto result = tree_.Seek(key);
  if (!result) {
    return Err(context_.Fail(std::move(result.error())));
  }
  return result;
}

auto WriteTransaction::Commit() -> Status {
  const auto writer = std::move(writer_lock_);
  const auto status = context_.CheckActive();
  state_.phase = detail::WriteState::Phase::Finished;
  if (!status.Ok()) {
    return status;
  }
  return database_.Commit(state_);
}

} // namespace tinydb
