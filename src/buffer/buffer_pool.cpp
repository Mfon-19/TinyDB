#include <tinydb/buffer_pool.h>
#include <tinydb/check.h>

#include <cstdio>
#include <expected>
#include <utility>

namespace tinydb {

BufferPool::BufferPool(DiskManager *disk_manager, std::size_t frame_count)
    : disk_manager_(disk_manager), frames_(frame_count) {
  for (frame_id_t frame_id = 0; frame_id < frames_.size(); ++frame_id) {
    free_list_.push_back(frame_id);
  }
}

BufferPool::BufferPool(BufferPool &&other) noexcept
    : disk_manager_(other.disk_manager_),
      frames_(std::move(other.frames_)),
      page_table_(std::move(other.page_table_)),
      free_list_(std::move(other.free_list_)),
      next_victim_(other.next_victim_) {
  other.disk_manager_ = nullptr;
}

auto BufferPool::operator=(BufferPool &&other) noexcept -> BufferPool & {
  if (this != &other) {
    FlushBestEffort();

    disk_manager_ = other.disk_manager_;
    frames_ = std::move(other.frames_);
    page_table_ = std::move(other.page_table_);
    free_list_ = std::move(other.free_list_);
    next_victim_ = other.next_victim_;

    other.disk_manager_ = nullptr;
  }

  return *this;
}

BufferPool::~BufferPool() { FlushBestEffort(); }

void BufferPool::FlushBestEffort() noexcept {
  if (const auto status = FlushAllPages(); !status.Ok()) {
    std::fprintf(stderr, "tinydb: buffer pool flush failed: %s\n", status.ToString().c_str());
  }
}

auto BufferPool::NewPage() -> Result<NewPageResult> {
  const auto frame_id = PickFrame();
  if (!frame_id) {
    return std::unexpected(frame_id.error());
  }
  const auto new_page_id = disk_manager_->AllocatePage();
  if (!new_page_id) {
    return std::unexpected(new_page_id.error());
  }
  auto &frame = frames_[*frame_id];

  frame.page_id = *new_page_id;
  frame.data.fill(0);
  frame.pin_count = 1;
  frame.dirty = true;
  page_table_[*new_page_id] = *frame_id;

  return NewPageResult{.page_id = *new_page_id, .data = frame.data.data()};
}

auto BufferPool::FetchPage(page_id_t page_id) -> Result<char *> {
  const auto page_it = page_table_.find(page_id);
  if (page_it != page_table_.end()) {
    auto &frame = frames_[page_it->second];
    ++frame.pin_count;
    return frame.data.data();
  }

  const auto frame_id = PickFrame();
  if (!frame_id) {
    return std::unexpected(frame_id.error());
  }
  auto &frame = frames_[*frame_id];

  if (auto status = disk_manager_->ReadPage(page_id, frame.data.data()); !status.Ok()) {
    // The picked frame is left clean, unpinned, and out of the page table;
    // the next eviction scan can reuse it.
    return std::unexpected(std::move(status));
  }
  frame.page_id = page_id;
  frame.pin_count = 1;
  frame.dirty = false;
  page_table_[page_id] = *frame_id;

  return frame.data.data();
}

void BufferPool::UnpinPage(page_id_t page_id, bool dirty) {
  const auto page_it = page_table_.find(page_id);
  TINYDB_CHECK(page_it != page_table_.end(), "unpinning a page that is not in the pool");

  auto &frame = frames_[page_it->second];
  TINYDB_CHECK(frame.pin_count > 0, "unpinning an unpinned page");

  --frame.pin_count;
  frame.dirty = frame.dirty || dirty;
}

auto BufferPool::FreePage(page_id_t page_id) -> Status {
  const auto page_it = page_table_.find(page_id);
  if (page_it != page_table_.end()) {
    auto &frame = frames_[page_it->second];
    TINYDB_CHECK(frame.pin_count == 0, "freeing a pinned page");

    frame.page_id = HEADER_PAGE_ID;
    frame.dirty = false;
    free_list_.push_back(page_it->second);
    page_table_.erase(page_it);
  }

  return disk_manager_->FreePage(page_id);
}

auto BufferPool::FlushPage(page_id_t page_id) -> Status {
  const auto page_it = page_table_.find(page_id);
  if (page_it == page_table_.end()) {
    return {};
  }

  auto &frame = frames_[page_it->second];
  if (frame.dirty) {
    if (auto status = disk_manager_->WritePage(frame.page_id, frame.data.data()); !status.Ok()) {
      return status;
    }
    frame.dirty = false;
  }
  return {};
}

auto BufferPool::FlushAllPages() -> Status {
  if (disk_manager_ == nullptr) {
    return {};
  }

  // Frames are marked clean as they are written, so a failed flush can be
  // retried and only rewrites the remainder.
  for (auto &frame : frames_) {
    if (frame.dirty) {
      if (auto status = disk_manager_->WritePage(frame.page_id, frame.data.data()); !status.Ok()) {
        return status;
      }
      frame.dirty = false;
    }
  }
  return {};
}

auto BufferPool::PickFrame() -> Result<frame_id_t> {
  if (!free_list_.empty()) {
    const auto frame_id = free_list_.back();
    free_list_.pop_back();
    return frame_id;
  }

  for (std::size_t count = 0; count < frames_.size(); ++count) {
    const auto frame_id = next_victim_;
    next_victim_ = (next_victim_ + 1) % frames_.size();
    auto &frame = frames_[frame_id];

    if (frame.pin_count == 0) {
      if (frame.dirty) {
        if (auto status = disk_manager_->WritePage(frame.page_id, frame.data.data()); !status.Ok()) {
          return std::unexpected(std::move(status));
        }
      }
      page_table_.erase(frame.page_id);
      frame.dirty = false;
      return frame_id;
    }
  }

  return std::unexpected(Status::ResourceExhausted("no evictable frame"));
}

}  // namespace tinydb
