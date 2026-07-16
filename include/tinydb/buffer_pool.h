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
  bool op_dirty{false};
};

struct NewPageResult {
  page_id_t page_id;
  char *data;
};

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

  void BeginOp();
  auto OpDirtyFrames() const -> std::vector<std::pair<page_id_t, const char *>>;
  void EndOp();

  void FreePage(page_id_t page_id);
  auto FlushPage(page_id_t page_id) -> Status;
  auto FlushAllPages() -> Status;

 private:
  auto PickFrame() -> Result<frame_id_t>;
  void FlushBestEffort() noexcept;

  DiskManager *disk_manager_{nullptr};
  std::vector<Frame> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;
  frame_id_t next_victim_{0};
  bool in_op_{false};
};

}  // namespace tinydb
