#include "tinydb/detail/page_context.h"
#include "tinydb/cache/buffer_pool.h"
#include <cassert>
#include <utility>

namespace tinydb::detail {

bool PageContext::Active() const noexcept {
  return !poisoned_ && (!write_ || write_->phase == WriteState::Phase::Active);
}

Status PageContext::CheckActive() const {
  if (poisoned_) {
    return Status::IoError(POISONED_DATABASE_MESSAGE);
  }
  if (write_ && write_->phase != WriteState::Phase::Active) {
    return Status::InvalidArgument("write transaction is not active");
  }
  return {};
}

Result<storage::PageBytes> PageContext::ReadPage(storage::PageId page_id) {
  if (auto status = CheckActive(); !status.Ok()) {
    return Err(std::move(status));
  }
  if (write_) {
    if (auto found = write_->pages.find(page_id);
        found != write_->pages.end()) {
      return found->second;
    }
  }
  auto page = pool_.ReadPage(page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }
  return page->Bytes();
}

Status PageContext::WritePage(storage::PageId page_id,
                              const storage::PageBytes &page) {
  assert(write_);
  if (auto status = CheckActive(); !status.Ok()) {
    return status;
  }
  assert(page_id > 0 && page_id < write_->page_count);
  write_->pages.insert_or_assign(page_id, page);
  return {};
}

Result<storage::PageId> PageContext::AllocatePage() {
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
}

} // namespace tinydb::detail
