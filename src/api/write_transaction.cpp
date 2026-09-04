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

void WriteTransaction::Finish() {
  state_.phase = detail::WriteState::Phase::Finished;
  state_.pages.clear();
  state_.free_pages.clear();
  if (writer_lock_.owns_lock()) {
    writer_lock_.unlock();
  }
}

Status WriteTransaction::Commit() {
  if (auto status = context_.CheckActive(); !status.Ok()) {
    Finish();
    return status;
  }

  for (const auto &[page_id, page] : state_.pages) {
    if (auto status = database_.buffer_pool_.WritePage(page_id, page);
        !status.Ok()) {
      database_.poisoned_ = true;
      Finish();
      return status;
    }
  }
  database_.page_count_ = state_.page_count;
  database_.free_pages_ = std::move(state_.free_pages);
  Finish();
  return {};
}

} // namespace tinydb
