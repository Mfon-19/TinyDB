#include "tinydb/cache/buffer_pool.h"
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager)
    : disk_manager_(std::move(disk_manager)) {}

auto BufferPool::FindFrame(storage::PageId page_id) -> Frame * {
  for (auto &frame : frames_) {
    if (frame.page_id == page_id) {
      return &frame;
    }
  }

  return nullptr;
}

Status BufferPool::WritePage(storage::PageId page_id,
                             const storage::PageBytes &page) {
  auto status = disk_manager_.WritePage(page_id, page);
  if (!status.Ok()) {
    return status;
  }

  auto *frame = FindFrame(page_id);
  if (frame == nullptr) {
    frames_.push_back(Frame{page_id, page});
  } else {
    frame->page = page;
  }

  return {};
}

Status BufferPool::ReadPage(storage::PageId page_id, storage::PageBytes &page) {
  if (auto *frame = FindFrame(page_id); frame != nullptr) {
    page = frame->page;
    return {};
  }

  Frame frame{page_id, {}};
  auto status = disk_manager_.ReadPage(page_id, frame.page);
  if (!status.Ok()) {
    return status;
  }

  frames_.push_back(std::move(frame));
  page = frames_.back().page;
  return {};
}

} // namespace tinydb::cache
