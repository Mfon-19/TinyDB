#include <tinydb/buffer_pool.h>
#include <tinydb/check.h>

#include <stdexcept>
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
    FlushAllPages();

    disk_manager_ = other.disk_manager_;
    frames_ = std::move(other.frames_);
    page_table_ = std::move(other.page_table_);
    free_list_ = std::move(other.free_list_);
    next_victim_ = other.next_victim_;

    other.disk_manager_ = nullptr;
  }

  return *this;
}

BufferPool::~BufferPool() { FlushAllPages(); }

auto BufferPool::NewPage(page_id_t *page_id) -> char * {
  const auto frame_id = PickFrame();
  const auto new_page_id = disk_manager_->AllocatePage();
  auto &frame = frames_[frame_id];

  frame.page_id = new_page_id;
  frame.data.fill(0);
  frame.pin_count = 1;
  frame.dirty = true;
  page_table_[new_page_id] = frame_id;
  *page_id = new_page_id;

  return frame.data.data();
}

auto BufferPool::FetchPage(page_id_t page_id) -> char * {
  const auto page_it = page_table_.find(page_id);
  if (page_it != page_table_.end()) {
    auto &frame = frames_[page_it->second];
    ++frame.pin_count;
    return frame.data.data();
  }

  const auto frame_id = PickFrame();
  auto &frame = frames_[frame_id];

  disk_manager_->ReadPage(page_id, frame.data.data());
  frame.page_id = page_id;
  frame.pin_count = 1;
  frame.dirty = false;
  page_table_[page_id] = frame_id;

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

void BufferPool::FlushPage(page_id_t page_id) {
  const auto page_it = page_table_.find(page_id);
  if (page_it == page_table_.end()) {
    return;
  }

  auto &frame = frames_[page_it->second];
  if (frame.dirty) {
    disk_manager_->WritePage(frame.page_id, frame.data.data());
    frame.dirty = false;
  }
}

void BufferPool::FlushAllPages() {
  if (disk_manager_ == nullptr) {
    return;
  }

  for (auto &frame : frames_) {
    if (frame.dirty) {
      disk_manager_->WritePage(frame.page_id, frame.data.data());
      frame.dirty = false;
    }
  }
}

auto BufferPool::PickFrame() -> frame_id_t {
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
        disk_manager_->WritePage(frame.page_id, frame.data.data());
      }
      page_table_.erase(frame.page_id);
      frame.dirty = false;
      return frame_id;
    }
  }

  throw std::runtime_error("no evictable frame");
}

}  // namespace tinydb
