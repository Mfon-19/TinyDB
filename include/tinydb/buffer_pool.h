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

struct Frame {
  page_id_t page_id{HEADER_PAGE_ID};
  std::array<char, PAGE_SIZE> data{};
  std::size_t pin_count{0};
  bool dirty{false};

  // Dirtied by the operation in flight (see BeginOp/EndOp). Such a frame is
  // never evicted and never flushed: its bytes are uncommitted until the
  // engine's WAL commit, so writing them to the database file would break
  // the no-steal rule the redo-only log depends on.
  bool op_dirty{false};
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

  // Brackets one engine operation for write-ahead logging. Every frame
  // dirtied between BeginOp and EndOp is marked op_dirty and pinned to the
  // pool in that state: unevictable and unflushable until its image is
  // durable in the log.
  void BeginOp();

  // The (page_id, bytes) of every frame the in-flight operation dirtied —
  // exactly the images the engine must log before committing. The pointers
  // are valid until the pool fetches or allocates another page.
  auto OpDirtyFrames() const -> std::vector<std::pair<page_id_t, const char *>>;

  // Ends the operation begun by BeginOp. Only call once the logged images
  // are durable (or abandoned to a poisoned engine): the frames become
  // ordinary dirty pages again, free to evict or flush.
  void EndOp();

  // Drops page_id from the pool (it must be unpinned; its cached bytes are
  // dead, so they are discarded rather than flushed) and puts it on the disk
  // manager's free list for reuse.
  void FreePage(page_id_t page_id);
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

  // Inside a BeginOp/EndOp bracket. Dirtying a frame only sets op_dirty
  // while this is true; open-time bootstrap writes stay plain dirty.
  bool in_op_{false};
};

}  // namespace tinydb
