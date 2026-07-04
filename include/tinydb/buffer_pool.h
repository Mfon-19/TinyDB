#pragma once

#include <tinydb/disk_manager.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace tinydb {

using frame_id_t = std::size_t;

struct Frame {
  page_id_t page_id{HEADER_PAGE_ID};
  std::array<char, PAGE_SIZE> data{};
  std::size_t pin_count{0};
  bool dirty{false};
};

// What NewPage hands back: the id of the freshly allocated page and its
// zeroed in-pool bytes, already pinned.
struct NewPageResult {
  page_id_t page_id;
  char *data;
};

// BufferPool caches a fixed number of database pages in memory.
class BufferPool {
 public:
  BufferPool(DiskManager *disk_manager, std::size_t frame_count);

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;

  BufferPool(BufferPool &&other) noexcept;
  auto operator=(BufferPool &&other) noexcept -> BufferPool &;
  ~BufferPool();

  auto NewPage() -> Result<NewPageResult>;
  auto FetchPage(page_id_t page_id) -> Result<char *>;
  void UnpinPage(page_id_t page_id, bool dirty);

  // Drops page_id from the pool (it must be unpinned; its cached bytes are
  // dead, so they are discarded rather than flushed) and puts it on the disk
  // manager's free list for reuse.
  auto FreePage(page_id_t page_id) -> Status;
  auto FlushPage(page_id_t page_id) -> Status;
  auto FlushAllPages() -> Status;

 private:
  // Hands out a free frame, evicting an unpinned page if none are free.
  // Fails with ResourceExhausted when every frame is pinned.
  auto PickFrame() -> Result<frame_id_t>;

  // FlushAllPages for the destructor and move assignment, which cannot
  // surface a status: failures are reported to stderr and dropped. Callers
  // who need to handle flush errors flush through the owning handle first.
  void FlushBestEffort() noexcept;

  DiskManager *disk_manager_{nullptr};
  std::vector<Frame> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;
  frame_id_t next_victim_{0};
};

}  // namespace tinydb
