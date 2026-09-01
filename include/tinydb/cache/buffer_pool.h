#pragma once

/*
 * A BufferPool keeps pages read from disk into in-memory
 * fixed-sized frames.
 */

#include "tinydb/cache/page_handle.h"
#include "tinydb/storage/disk_manager.h"
#include <cstddef>
#include <unordered_map>

namespace tinydb::cache {

class BufferPool {
public:
  BufferPool(storage::DiskManager disk_manager, std::size_t capacity);

  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Result<PageHandle> ReadPage(storage::PageId page_id);

private:
  struct Frame {
    storage::PageBytes page;
    std::size_t pin_count = 0;
  };

  storage::DiskManager disk_manager_;
  std::size_t capacity_;
  std::unordered_map<storage::PageId, Frame> frames_;
};

} // namespace tinydb::cache
