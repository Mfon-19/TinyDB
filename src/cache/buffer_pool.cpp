#include "tinydb/cache/buffer_pool.h"
#include <algorithm>
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager, std::size_t capacity)
    : disk_manager_(std::move(disk_manager)), capacity_(capacity),
      frames_(capacity),
      buckets_(std::max(capacity, std::size_t{1}), frames_.end()) {}

auto BufferPool::FindPage(storage::PageId page_id) -> FrameIterator {
  auto frame = buckets_[page_id % buckets_.size()];
  while (frame != frames_.end() && frame->page_id != page_id) {
    frame = frame->hash_next;
  }
  return frame;
}

void BufferPool::SetPageId(FrameIterator frame, storage::PageId page_id) {
  if (frame->page_id != storage::INVALID_PAGE_ID) {
    auto *link = &buckets_[frame->page_id % buckets_.size()];
    while (*link != frame) {
      link = &(*link)->hash_next;
    }
    *link = frame->hash_next;
  }
  auto &head = buckets_[page_id % buckets_.size()];
  frame->hash_next = head;
  head = frame;
  frame->page_id = page_id;
}

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

void BufferPool::Touch(FrameIterator frame) {
  frames_.splice(frames_.begin(), frames_, frame);
}

Status BufferPool::InstallPage(storage::PageId page_id,
                               const storage::PageBytes &page) {
  if (page_id == storage::INVALID_PAGE_ID) {
    return Status::InvalidArgument("invalid page ID");
  }
  std::lock_guard lock(mutex_);
  auto frame = FindPage(page_id);
  if (frame != frames_.end() &&
      frame->pin_count.load(std::memory_order_acquire) != 0) {
    return Status::ResourceExhausted("cannot write a pinned page");
  }
  if (frame == frames_.end()) {
    frame = FindVictim();
    if (frame == frames_.end()) {
      return Status::ResourceExhausted(
          "all buffer pool frames are dirty or pinned");
    }
  }

  SetPageId(frame, page_id);
  frame->page = page;
  frame->dirty = true;
  Touch(frame);
  return {};
}

Result<PageHandle> BufferPool::ReadPage(storage::PageId page_id) {
  if (page_id == storage::INVALID_PAGE_ID) {
    return Err(Status::InvalidArgument("invalid page ID"));
  }
  std::lock_guard lock(mutex_);
  if (auto frame = FindPage(page_id); frame != frames_.end()) {
    Touch(frame);
    return PageHandle{&frame->page, &frame->pin_count};
  }

  auto frame = FindVictim();
  if (frame == frames_.end()) {
    return Err(Status::ResourceExhausted(
        "all buffer pool frames are dirty or pinned"));
  }

  storage::PageBytes page{};
  auto status = disk_manager_.ReadPage(page_id, page);
  if (!status.Ok()) {
    return Err(std::move(status));
  }

  SetPageId(frame, page_id);
  frame->page = page;
  Touch(frame);
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
