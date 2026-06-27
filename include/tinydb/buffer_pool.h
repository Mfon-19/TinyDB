#include <tinydb/disk_manager.h>

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

// BufferPool caches a fixed number of database pages in memory.
class BufferPool {
 public:
  BufferPool(DiskManager *disk_manager, std::size_t frame_count);

  BufferPool(const BufferPool &) = delete;
  auto operator=(const BufferPool &) -> BufferPool & = delete;

  BufferPool(BufferPool &&other) noexcept;
  auto operator=(BufferPool &&other) noexcept -> BufferPool &;
  ~BufferPool();

  auto NewPage(page_id_t *page_id) -> char *;
  auto FetchPage(page_id_t page_id) -> char *;
  void UnpinPage(page_id_t page_id, bool dirty);
  void FlushPage(page_id_t page_id);
  void FlushAllPages();

 private:
  auto PickFrame() -> frame_id_t;

  DiskManager *disk_manager_{nullptr};
  std::vector<Frame> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;
  frame_id_t next_victim_{0};
};

}  // namespace tinydb
