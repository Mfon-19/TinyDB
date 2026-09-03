#include "tinydb/cache/buffer_pool.h"
#include <cassert>
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager, std::size_t capacity)
    : disk_manager_(std::move(disk_manager)), capacity_(capacity) {}

auto BufferPool::PageCount() const noexcept -> storage::PageId {
  return disk_manager_.PageCount();
}

Result<storage::PageId> BufferPool::AllocatePage() {
  if (!free_pages_.empty()) {
    const storage::PageId page_id = free_pages_.back();
    free_pages_.pop_back();
    return page_id;
  }
  return disk_manager_.AllocatePage();
}

void BufferPool::FreePage(storage::PageId page_id) {
  assert(page_id != 0);
  assert(page_id != storage::INVALID_PAGE_ID);
  if (auto found = page_table_.find(page_id); found != page_table_.end()) {
    assert(found->second->pin_count == 0);
    Evict(found->second);
  }
  free_pages_.push_back(page_id);
}

void BufferPool::SetFreePages(std::vector<storage::PageId> page_ids) {
  free_pages_ = std::move(page_ids);
}

auto BufferPool::FindVictim() -> FrameIterator {
  auto frame = frames_.end();
  while (frame != frames_.begin()) {
    --frame;
    if (frame->pin_count == 0) {
      return frame;
    }
  }

  return frames_.end();
}

auto BufferPool::InsertFrame(storage::PageId page_id,
                             storage::PageBytes page) -> FrameIterator {
  frames_.push_front(Frame{page_id, std::move(page)});
  page_table_.emplace(page_id, frames_.begin());
  return frames_.begin();
}

void BufferPool::Touch(FrameIterator frame) {
  frames_.splice(frames_.begin(), frames_, frame);
}

void BufferPool::Evict(FrameIterator frame) {
  page_table_.erase(frame->page_id);
  frames_.erase(frame);
}

Status BufferPool::WritePage(storage::PageId page_id,
                             const storage::PageBytes &page) {
  auto found = page_table_.find(page_id);
  auto victim = frames_.end();
  if (found == page_table_.end() && frames_.size() >= capacity_) {
    victim = FindVictim();
    if (victim == frames_.end()) {
      return Status::ResourceExhausted("all buffer pool frames are pinned");
    }
  }

  auto status = disk_manager_.WritePage(page_id, page);
  if (!status.Ok()) {
    return status;
  }

  if (found == page_table_.end()) {
    if (victim != frames_.end()) {
      Evict(victim);
    }
    InsertFrame(page_id, page);
  } else {
    found->second->page = page;
    Touch(found->second);
  }

  return {};
}

Result<PageHandle> BufferPool::ReadPage(storage::PageId page_id) {
  if (auto found = page_table_.find(page_id); found != page_table_.end()) {
    Touch(found->second);
    return PageHandle{&found->second->page, &found->second->pin_count};
  }

  auto victim = frames_.end();
  if (frames_.size() >= capacity_) {
    victim = FindVictim();
    if (victim == frames_.end()) {
      return Err(
          Status::ResourceExhausted("all buffer pool frames are pinned"));
    }
  }

  storage::PageBytes page{};
  auto status = disk_manager_.ReadPage(page_id, page);
  if (!status.Ok()) {
    return Err(std::move(status));
  }

  if (victim != frames_.end()) {
    Evict(victim);
  }
  auto frame = InsertFrame(page_id, std::move(page));
  return PageHandle{&frame->page, &frame->pin_count};
}

} // namespace tinydb::cache
