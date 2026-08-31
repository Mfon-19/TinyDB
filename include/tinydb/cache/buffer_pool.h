#pragma once

/*
 * A BufferPool keeps pages read from disk into in-memory
 * fixed-sized frames.
 */

#include "tinydb/storage/disk_manager.h"
#include <vector>

namespace tinydb::cache {

class BufferPool {
public:
  explicit BufferPool(storage::DiskManager disk_manager);

  Status WritePage(storage::PageId page_id, const storage::PageBytes &page);
  Status ReadPage(storage::PageId page_id, storage::PageBytes &page);

private:
  struct Frame {
    storage::PageId page_id;
    storage::PageBytes page;
  };

  auto FindFrame(storage::PageId page_id) -> Frame *;

  storage::DiskManager disk_manager_;
  std::vector<Frame> frames_;
};

} // namespace tinydb::cache
