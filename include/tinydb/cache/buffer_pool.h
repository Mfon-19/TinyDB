#pragma once

/*
 * A BufferPool keeps pages read from disk into in-memory
 * fixed-sized frames.
 */

#include "tinydb/storage/disk_manager.h"
#include <unordered_map>

namespace tinydb::cache {

class BufferPool {
public:
  explicit BufferPool(storage::DiskManager disk_manager);

  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Status ReadPage(storage::PageId page_id, storage::PageBytes &page);

private:
  struct Frame {
    storage::PageBytes page;
  };

  storage::DiskManager disk_manager_;
  std::unordered_map<storage::PageId, Frame> frames_;
};

} // namespace tinydb::cache
