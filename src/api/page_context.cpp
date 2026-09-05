#include "tinydb/detail/page_context.h"
#include "tinydb/cache/buffer_pool.h"
#include <cassert>
#include <memory>
#include <utility>

namespace tinydb::detail {

auto PageContext::Active() const noexcept -> bool {
  return !poisoned_ && (!write_ || write_->phase == WriteState::Phase::Active);
}

auto PageContext::Version() const noexcept -> std::uint64_t {
  return write_ ? write_->version : 0;
}

auto PageContext::CheckActive() const -> Status {
  if (poisoned_) {
    return Status::IoError(POISONED_DATABASE_MESSAGE);
  }
  if (write_ && write_->phase != WriteState::Phase::Active) {
    return Status::InvalidArgument("write transaction is not active");
  }
  return {};
}

auto PageContext::Fail(Status error) -> Status {
  assert(!error.Ok());
  if (write_ && write_->phase == WriteState::Phase::Active) {
    write_->phase = WriteState::Phase::Failed;
  }
  return error;
}

auto PageContext::ReadPage(storage::PageId page_id)
    -> Result<storage::PageRef> {
  if (auto status = CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  if (write_) {
    if (auto found = write_->pages.find(page_id);
        found != write_->pages.end()) {
      return std::make_shared<const storage::Page>(found->second);
    }
  }
  return pool_.ReadPage(page_id);
}

auto PageContext::WritePage(const storage::Page &page) -> Status {
  assert(write_);
  if (auto status = CheckActive(); !status.Ok()) {
    return status;
  }
  assert(page.Id() < write_->page_count);
  write_->pages.insert_or_assign(page.Id(), page);
  ++write_->version;
  return {};
}

auto PageContext::AllocatePage() -> Result<storage::PageId> {
  assert(write_);
  if (auto status = CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  if (!write_->free_pages.empty()) {
    const auto page_id = write_->free_pages.back();
    write_->free_pages.pop_back();
    return page_id;
  }
  if (write_->page_count == storage::INVALID_PAGE_ID) {
    return Err(Status::ResourceExhausted("database has too many pages"));
  }
  return write_->page_count++;
}

void PageContext::FreePage(storage::PageId page_id) {
  assert(write_ && Active());
  assert(page_id > 1 && page_id < write_->page_count);
  write_->free_pages.push_back(page_id);
  write_->pages.erase(page_id);
  ++write_->version;
}

} // namespace tinydb::detail
