#pragma once

/*
 * A BufferPool keeps pages read from disk into in-memory
 * fixed-sized frames. When full, it replaces the least recently
 * used frame that is neither dirty nor pinned by a PageHandle.
 * All frames are allocated at construction and reused.
 */

#include "tinydb/cache/page_handle.h"
#include "tinydb/storage/disk_manager.h"
#include <atomic>
#include <cstddef>
#include <list>
#include <map>
#include <mutex>

namespace tinydb::cache {

class BufferPool {
public:
  BufferPool(storage::DiskManager disk_manager, std::size_t capacity);

  BufferPool(const BufferPool &) = delete;
  BufferPool &operator=(const BufferPool &) = delete;
  BufferPool(BufferPool &&) = delete;
  BufferPool &operator=(BufferPool &&) = delete;

  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }
  Status InstallPage(storage::PageId page_id, const storage::PageBytes &page);
  Result<PageHandle> ReadPage(storage::PageId page_id);
  Status Flush(const std::map<storage::PageId, storage::PageBytes> &incoming);

private:
  struct Frame {
    storage::PageId page_id = storage::INVALID_PAGE_ID;
    storage::PageBytes page{};
    std::atomic<std::size_t> pin_count{0};
    bool dirty = false;
  };

  using FrameIterator = std::list<Frame>::iterator;

  auto FindVictim() -> FrameIterator;
  void Touch(FrameIterator frame);

  storage::DiskManager disk_manager_;
  std::size_t capacity_;
  std::mutex mutex_;
  std::list<Frame> frames_;
};

} // namespace tinydb::cache
