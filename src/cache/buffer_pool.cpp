#include "tinydb/cache/buffer_pool.h"
#include <algorithm>
#include <cassert>
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager, std::size_t capacity)
    : disk_manager_(std::move(disk_manager)), capacity_(capacity),
      frames_(capacity),
      buckets_(std::max(capacity, std::size_t{1}), frames_.end()) {}

auto BufferPool::FindPage(storage::PageId page_id) -> FrameIterator {
  auto frame = buckets_[page_id % buckets_.size()];
  while (frame != frames_.end() && frame->page->Id() != page_id) {
    frame = frame->hash_next;
  }
  return frame;
}

void BufferPool::SetPage(FrameIterator frame, const storage::Page &page) {
  if (frame->page) {
    auto *link = &buckets_[frame->page->Id() % buckets_.size()];
    while (*link != frame) {
      link = &(*link)->hash_next;
    }
    *link = frame->hash_next;
  }
  auto &head = buckets_[page.Id() % buckets_.size()];
  frame->hash_next = head;
  head = frame;
  frame->page = page;
}

auto BufferPool::FindVictim() -> FrameIterator {
  auto frame = frames_.end();
  while (frame != frames_.begin()) {
    --frame;
    if (!frame->dirty) {
      return frame;
    }
  }

  return frames_.end();
}

void BufferPool::Touch(FrameIterator frame) {
  frames_.splice(frames_.begin(), frames_, frame);
}

auto BufferPool::InstallPage(const storage::Page &page) -> Status {
  std::lock_guard lock(mutex_);
  auto frame = FindPage(page.Id());
  if (frame == frames_.end()) {
    frame = FindVictim();
    if (frame == frames_.end()) {
      return Status::ResourceExhausted("all buffer pool frames are dirty");
    }
  }

  SetPage(frame, page);
  frame->dirty = true;
  Touch(frame);
  return {};
}

auto BufferPool::ReadPage(storage::PageId page_id) -> Result<storage::Page> {
  if (!storage::ValidDataPageId(page_id)) {
    return Err(Status::InvalidArgument("invalid page ID"));
  }
  std::lock_guard lock(mutex_);
  if (auto frame = FindPage(page_id); frame != frames_.end()) {
    Touch(frame);
    return *frame->page;
  }

  auto frame = FindVictim();
  if (frame == frames_.end()) {
    return Err(Status::ResourceExhausted("all buffer pool frames are dirty"));
  }

  storage::PageBytes page;
  auto status = disk_manager_.ReadPage(page_id, page);
  if (!status.Ok()) {
    return Err(std::move(status));
  }

  auto decoded = storage::DecodePage(page_id, page);
  if (decoded) {
    SetPage(frame, *decoded);
    Touch(frame);
  }
  return decoded;
}

auto BufferPool::Checkpoint(const storage::PageMap &incoming) -> Status {
  std::lock_guard lock(mutex_);
  for (const auto &frame : frames_) {
    if (frame.dirty && !incoming.contains(frame.page->Id())) {
      if (auto status =
              disk_manager_.WritePage(frame.page->Id(), frame.page->Bytes());
          !status.Ok()) {
        return status;
      }
    }
  }
  for (const auto &[page_id, page] : incoming) {
    assert(page_id == page.Id());
    if (auto status = disk_manager_.WritePage(page_id, page.Bytes());
        !status.Ok()) {
      return status;
    }
  }
  if (auto status = disk_manager_.Sync(); !status.Ok()) {
    return status;
  }
  for (auto &frame : frames_) {
    if (!frame.page) {
      continue;
    }
    if (auto found = incoming.find(frame.page->Id()); found != incoming.end()) {
      frame.page = found->second;
    }
    frame.dirty = false;
  }
  return {};
}

} // namespace tinydb::cache
