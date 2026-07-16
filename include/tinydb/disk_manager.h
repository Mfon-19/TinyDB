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

// A physical page image suitable for redo logging. The bytes are already in
// their final on-disk representation, including identity and checksum, so WAL
// recovery never needs to invoke B+ tree or allocator logic.
struct PageImage {
  page_id_t page_id;
  std::array<char, PAGE_SIZE> data;
};

// Owns the database file and its persistent allocation metadata.
//
// The DiskManager deliberately does not decide when an operation commits. It
// keeps root/free-list/frontier changes in memory and exposes their final page
// images through TakeOpImages(); StorageEngine places those images in the WAL
// alongside dirty tree pages. Checkpoint() is the separate path that writes
// committed metadata into the database file once the WAL is authoritative.
class DiskManager {
 public:
  static auto Open(const std::filesystem::path &path) -> Result<DiskManager>;

  DiskManager(const DiskManager &) = delete;
  auto operator=(const DiskManager &) -> DiskManager & = delete;
  DiskManager(DiskManager &&) noexcept = default;
  auto operator=(DiskManager &&) noexcept -> DiskManager & = default;
  ~DiskManager() = default;

  auto AllocatePage() -> Result<page_id_t>;

  // FreePage is a logical allocator update. It does not immediately overwrite
  // the page on disk; doing so could expose an uncommitted free-list link.
  void FreePage(page_id_t page_id);

  auto GetRootPageId() const -> page_id_t;
  auto NextPageId() const -> page_id_t;
  auto FreePages() const -> const std::unordered_set<page_id_t> &;
  void SetRootPageId(page_id_t root_page_id);
  auto Uuid() const -> const DatabaseUuid &;

  // Drains metadata images produced by the current engine operation. Each
  // image must be appended before that operation's WAL commit record.
  auto TakeOpImages() -> std::vector<PageImage>;

  // Materializes committed deferred allocator links and mirrors the current
  // superblock. The caller must fsync the database before resetting the WAL.
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

  // Logical metadata represented by the newest selected superblock plus any
  // changes made since it was read. EncodeCurrentSuperblock snapshots these
  // fields atomically into one page image.
  DatabaseUuid database_uuid_{};
  std::uint64_t generation_{1};
  std::uint64_t checkpoint_lsn_{0};
  std::uint64_t transaction_id_{0};
  page_id_t root_page_id_{HEADER_PAGE_ID};
  page_id_t next_page_id_{FIRST_DATA_PAGE_ID};
  page_id_t free_list_head_{HEADER_PAGE_ID};
  page_id_t active_superblock_page_id_{SUPERBLOCK_A_PAGE_ID};

  // Complete membership set used for fast validation/double-free detection.
  // The ordering of reusable pages is carried separately by free_list_head_
  // and the encoded next links.
  std::unordered_set<page_id_t> free_pages_;

  // New free-list links remain private here until logged or checkpointed.
  // A page can be allocated again during the same operation, in which case
  // its pending link is removed without ever touching the database file.
  std::unordered_map<page_id_t, page_id_t> pending_free_links_;

  // Subset of pending links introduced by the operation currently being
  // committed. TakeOpImages drains this list exactly once.
  std::vector<page_id_t> op_freed_pages_;

  // Root, frontier, or free-list head changed and therefore requires a new
  // alternating superblock generation in the next logged operation.
  bool header_changed_{false};
};

}  // namespace tinydb
