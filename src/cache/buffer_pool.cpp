#include "tinydb/cache/buffer_pool.h"
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager, std::size_t capacity)
    : disk_manager_(std::move(disk_manager)), capacity_(capacity) {}

auto BufferPool::FindVictim() -> FrameIterator {
  auto frame = frames_.end();
  while (frame != frames_.begin()) {
    --frame;
    if (!frame->dirty &&
        frame->pin_count.load(std::memory_order_acquire) == 0) {
      return frame;
    }
  }

  return frames_.end();
}

auto BufferPool::InsertFrame(storage::PageId page_id,
                             storage::PageBytes page) -> FrameIterator {
  frames_.emplace_front(page_id, std::move(page));
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

Status BufferPool::InstallPage(storage::PageId page_id,
                               const storage::PageBytes &page) {
  std::lock_guard lock(mutex_);
  auto found = page_table_.find(page_id);
  if (found != page_table_.end() &&
      found->second->pin_count.load(std::memory_order_acquire) != 0) {
    return Status::ResourceExhausted("cannot write a pinned page");
  }
  auto victim = frames_.end();
  if (found == page_table_.end() && frames_.size() >= capacity_) {
    victim = FindVictim();
    if (victim == frames_.end()) {
      return Status::ResourceExhausted(
          "all buffer pool frames are dirty or pinned");
    }
  }

  if (found == page_table_.end()) {
    if (victim != frames_.end()) {
      Evict(victim);
    }
    InsertFrame(page_id, page)->dirty = true;
  } else {
    found->second->page = page;
    found->second->dirty = true;
    Touch(found->second);
  }

  return {};
}

Result<PageHandle> BufferPool::ReadPage(storage::PageId page_id) {
  std::lock_guard lock(mutex_);
  if (auto found = page_table_.find(page_id); found != page_table_.end()) {
    Touch(found->second);
    return PageHandle{&found->second->page, &found->second->pin_count};
  }

  auto victim = frames_.end();
  if (frames_.size() >= capacity_) {
    victim = FindVictim();
    if (victim == frames_.end()) {
      return Err(Status::ResourceExhausted(
          "all buffer pool frames are dirty or pinned"));
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

Status BufferPool::Flush(
    const std::map<storage::PageId, storage::PageBytes> &incoming) {
  std::lock_guard lock(mutex_);
  for (const auto &frame : frames_) {
    if (frame.pin_count.load(std::memory_order_acquire) != 0) {
      return Status::ResourceExhausted("cannot checkpoint pinned pages");
    }
  }
  for (const auto &frame : frames_) {
    if (frame.dirty && !incoming.contains(frame.page_id)) {
      if (auto status = disk_manager_.WritePage(frame.page_id, frame.page);
          !status.Ok()) {
        return status;
      }
    }
  }
  for (const auto &[page_id, page] : incoming) {
    if (auto status = disk_manager_.WritePage(page_id, page); !status.Ok()) {
      return status;
    }
  }
  if (auto status = disk_manager_.Sync(); !status.Ok()) {
    return status;
  }
  for (auto &frame : frames_) {
    if (auto found = incoming.find(frame.page_id); found != incoming.end()) {
      frame.page = found->second;
    }
    frame.dirty = false;
  }
  return {};
}

} // namespace tinydb::cache
