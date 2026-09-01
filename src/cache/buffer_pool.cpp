#include "tinydb/cache/buffer_pool.h"
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager, std::size_t capacity)
    : disk_manager_(std::move(disk_manager)), capacity_(capacity) {}

Status BufferPool::WritePage(storage::PageId page_id,
                             const storage::PageBytes &page) {
  auto frame = frames_.find(page_id);
  if (frame == frames_.end() && frames_.size() >= capacity_) {
    return Status::ResourceExhausted("buffer pool is full");
  }

  auto status = disk_manager_.WritePage(page_id, page);
  if (!status.Ok()) {
    return status;
  }

  if (frame == frames_.end()) {
    frames_.emplace(page_id, Frame{page});
  } else {
    frame->second.page = page;
  }

  return {};
}

Result<PageHandle> BufferPool::ReadPage(storage::PageId page_id) {
  if (auto frame = frames_.find(page_id); frame != frames_.end()) {
    return PageHandle{&frame->second.page, &frame->second.pin_count};
  }

  if (frames_.size() >= capacity_) {
    return Err(Status::ResourceExhausted("buffer pool is full"));
  }

  Frame frame{};
  auto status = disk_manager_.ReadPage(page_id, frame.page);
  if (!status.Ok()) {
    return Err(std::move(status));
  }

  auto stored = frames_.emplace(page_id, std::move(frame)).first;
  return PageHandle{&stored->second.page, &stored->second.pin_count};
}

} // namespace tinydb::cache
