#pragma once

#include <tinydb/disk_manager.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tinydb {

using frame_id_t = std::size_t;

// One cache frame. `dirty` means the database file is behind this memory
// image. `op_dirty` is stronger: the image belongs to an operation whose WAL
// commit has not completed, so it may be neither flushed nor evicted.
struct Frame {
  page_id_t page_id{HEADER_PAGE_ID};
  std::array<char, PAGE_SIZE> data{};
  std::size_t pin_count{0};
  bool dirty{false};
  bool op_dirty{false};
};

// New pages are returned already pinned. The caller initializes the bytes and
// must eventually UnpinPage with dirty=true (normally through PageHandle's
// buffer-pool adapter).
struct NewPageResult {
  page_id_t page_id;
  char *data;
};

// Fixed-size page cache with pin-based lifetime and clock eviction.
//
// BeginOp/EndOp form the current engine's no-steal transaction bracket. This
// mechanism is intentionally temporary: Milestones 4–5 replace mutable shared
// frames with immutable committed frames and a private write overlay.
class BufferPool {
 public:
  BufferPool(DiskManager *disk_manager, std::size_t frame_count);

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;

  BufferPool(BufferPool &&other) noexcept;
  auto operator=(BufferPool &&other) noexcept -> BufferPool &;
  ~BufferPool();

  auto NewPage() -> Result<NewPageResult>;

  // A successful fetch returns a stable pointer until the matching unpin.
  auto FetchPage(page_id_t page_id) -> Result<char *>;
  void UnpinPage(page_id_t page_id, bool dirty);

  void BeginOp();

  // Borrowed pointers are valid only while the operation remains open and no
  // API contract permits another operation to begin concurrently.
  auto OpDirtyFrames() const -> std::vector<std::pair<page_id_t, const char *>>;
  void EndOp();

  void FreePage(page_id_t page_id);
  auto FlushPage(page_id_t page_id) -> Status;
  auto FlushAllPages() -> Status;

 private:
  // Chooses a free frame or an unpinned, non-op-dirty clock victim.
  auto PickFrame() -> Result<frame_id_t>;
  void FlushBestEffort() noexcept;

  DiskManager *disk_manager_{nullptr};
  std::vector<Frame> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;

  // Contains never-used frames and frames whose previous page was freed.
  std::vector<frame_id_t> free_list_;
  frame_id_t next_victim_{0};
  bool in_op_{false};
};

}  // namespace tinydb
