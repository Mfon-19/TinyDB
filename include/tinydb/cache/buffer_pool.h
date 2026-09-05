#pragma once

/*
 * A BufferPool keeps pages read from disk into in-memory
 * fixed-sized frames. When full, it replaces the least recently
 * used clean frame.
 * All frames are allocated at construction and reused.
 */

#include "tinydb/storage/disk_manager.h"
#include "tinydb/storage/page_codec.h"
#include <cstddef>
#include <list>
#include <mutex>
#include <vector>

namespace tinydb::cache {

class BufferPool {
public:
  BufferPool(storage::DiskManager disk_manager, std::size_t capacity);

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;
  BufferPool(BufferPool &&) = delete;
  auto operator=(BufferPool &&) -> BufferPool & = delete;

  [[nodiscard]] auto Capacity() const noexcept -> std::size_t {
    return capacity_;
  }
  auto InstallPage(const storage::Page &page) -> Status;
  [[nodiscard]] auto ReadPage(storage::PageId page_id)
      -> Result<storage::PageRef>;

  auto Checkpoint(const storage::PageMap &incoming) -> Status;

private:
  struct Frame;
  using FrameIterator = std::list<Frame>::iterator;

  struct Frame {
    storage::PageRef page;
    bool dirty = false;
    FrameIterator hash_next;
  };

  auto FindPage(storage::PageId page_id) -> FrameIterator;
  void SetPage(FrameIterator frame, storage::PageRef page);
  auto FindVictim() -> FrameIterator;
  void Touch(FrameIterator frame);

  storage::DiskManager disk_manager_;
  const std::size_t capacity_;

  std::mutex mutex_;
  std::list<Frame> frames_;
  std::vector<FrameIterator> buckets_;
};

} // namespace tinydb::cache
