#include "tinydb/cache/buffer_pool.h"
#include <utility>

namespace tinydb::cache {

BufferPool::BufferPool(storage::DiskManager disk_manager)
    : disk_manager_(std::move(disk_manager)) {}

Status BufferPool::WritePage(storage::PageId page_id,
                             const storage::PageBytes &page) {
  auto status = disk_manager_.WritePage(page_id, page);
  if (!status.Ok()) {
    return status;
  }

  frames_.insert_or_assign(page_id, Frame{page});

  return {};
}

Status BufferPool::ReadPage(storage::PageId page_id, storage::PageBytes &page) {
  if (auto frame = frames_.find(page_id); frame != frames_.end()) {
    page = frame->second.page;
    return {};
  }

  Frame frame{};
  auto status = disk_manager_.ReadPage(page_id, frame.page);
  if (!status.Ok()) {
    return status;
  }

  auto stored = frames_.emplace(page_id, std::move(frame)).first;
  page = stored->second.page;
  return {};
}

} // namespace tinydb::cache
