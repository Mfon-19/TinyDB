#pragma once

/*
 * A BufferPool keeps pages read from disk into in-memory
 * fixed-sized frames. When full, it replaces the least recently
 * used frame that is not pinned by a PageHandle.
 */

#include "tinydb/cache/page_handle.h"
#include "tinydb/storage/disk_manager.h"
#include <cstddef>
#include <list>
#include <unordered_map>

namespace tinydb::cache {

class BufferPool {
public:
  BufferPool(storage::DiskManager disk_manager, std::size_t capacity);

  Result<storage::PageId> AllocatePage();
  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Result<PageHandle> ReadPage(storage::PageId page_id);

private:
  struct Frame {
    storage::PageId page_id;
    storage::PageBytes page;
    std::size_t pin_count = 0;
  };

  using FrameIterator = std::list<Frame>::iterator;

  auto FindVictim() -> FrameIterator;
  auto InsertFrame(storage::PageId page_id,
                   storage::PageBytes page) -> FrameIterator;
  void Touch(FrameIterator frame);
  void Evict(FrameIterator frame);

  storage::DiskManager disk_manager_;
  std::size_t capacity_;
  std::list<Frame> frames_;
  std::unordered_map<storage::PageId, FrameIterator> page_table_;
};

} // namespace tinydb::cache
