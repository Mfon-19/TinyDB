#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {

struct PageImage {
  page_id_t page_id;
  std::array<char, PAGE_SIZE> data;
};

class DiskManager {
 public:
  static auto Open(const std::filesystem::path &path) -> Result<DiskManager>;

  DiskManager(const DiskManager &) = delete;
  auto operator=(const DiskManager &) -> DiskManager & = delete;
  DiskManager(DiskManager &&) noexcept = default;
  auto operator=(DiskManager &&) noexcept -> DiskManager & = default;
  ~DiskManager() = default;

  auto AllocatePage() -> Result<page_id_t>;
  void FreePage(page_id_t page_id);

  auto GetRootPageId() const -> page_id_t;
  auto NextPageId() const -> page_id_t;
  auto FreePages() const -> const std::unordered_set<page_id_t> &;
  void SetRootPageId(page_id_t root_page_id);
  auto Uuid() const -> const DatabaseUuid &;

  auto TakeOpImages() -> std::vector<PageImage>;
  auto Checkpoint() -> Status;
  auto Sync() const -> Status;

  auto ReadPage(page_id_t page_id, char *data) const -> Status;
  auto WritePage(page_id_t page_id, const char *data) const -> Status;

 private:
  explicit DiskManager(UniqueFd fd) : fd_(std::move(fd)) {}

  auto EncodeCurrentSuperblock() const -> std::array<char, PAGE_SIZE>;
  auto AdvanceSuperblock() -> page_id_t;
  auto WriteCurrentSuperblock() const -> Status;

  UniqueFd fd_;
  DatabaseUuid database_uuid_{};
  std::uint64_t generation_{1};
  std::uint64_t checkpoint_lsn_{0};
  std::uint64_t transaction_id_{0};
  page_id_t root_page_id_{HEADER_PAGE_ID};
  page_id_t next_page_id_{FIRST_DATA_PAGE_ID};
  page_id_t free_list_head_{HEADER_PAGE_ID};
  page_id_t active_superblock_page_id_{SUPERBLOCK_A_PAGE_ID};
  std::unordered_set<page_id_t> free_pages_;
  std::unordered_map<page_id_t, page_id_t> pending_free_links_;
  std::vector<page_id_t> op_freed_pages_;
  bool header_changed_{false};
};

}  // namespace tinydb
