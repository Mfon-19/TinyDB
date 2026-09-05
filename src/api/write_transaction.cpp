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
  if (auto status = context_.CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  auto result = tree_.Get(key);
  if (!result) {
    state_.phase = detail::WriteState::Phase::Failed;
  }
  return result;
}

Status WriteTransaction::Put(std::string_view key, std::string_view value) {
  if (auto status = context_.CheckActive(); !status.Ok()) {
    return status;
  }
  auto status = tree_.Put(key, value);
  if (!status.Ok()) {
    state_.phase = detail::WriteState::Phase::Failed;
  }
  return status;
}

auto WriteTransaction::Delete(std::string_view key) -> Result<bool> {
  if (auto status = context_.CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  auto result = tree_.Delete(key);
  if (!result) {
    state_.phase = detail::WriteState::Phase::Failed;
  }
  return result;
}

auto WriteTransaction::Seek(std::string_view key) -> Result<btree::Cursor> {
  if (auto status = context_.CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  auto result = tree_.Seek(key);
  if (!result) {
    state_.phase = detail::WriteState::Phase::Failed;
  }
  return result;
}

Status WriteTransaction::Commit() {
  auto writer = std::move(writer_lock_);
  auto status = context_.CheckActive();
  auto pending = std::move(state_);
  state_.phase = detail::WriteState::Phase::Finished;
  if (!status.Ok()) {
    return status;
  }
  return database_.Commit(pending);
}

} // namespace tinydb
