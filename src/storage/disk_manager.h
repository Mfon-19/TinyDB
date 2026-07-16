#pragma once

#include <tinydb/status.h>
#include "io/unique_fd.h"
#include "storage/database_uuid.h"
#include "storage/page.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace tinydb {

/*
** DISK MANAGER BOUNDARY
**
** DiskManager owns the database file descriptor and the metadata represented
** by the newest durable superblock. It performs physical page I/O and durable
** superblock selection; it does not track the newer state visible only through
** the committed cache and WAL. Logical allocation, retirement, and reuse
** belong to TransactionPages.
**
** Checkpoint data pages are written against an explicit captured high-water
** frontier. CommitCheckpoint then writes only the inactive superblock and
** synchronizes it before adopting the captured metadata in memory. Until that
** synchronization succeeds, the previously selected superblock and WAL remain
** the recovery basis. Superblocks are checkpoint artifacts; they are never
** transaction page images.
*/

class DiskManager {
 public:
  static auto Open(const std::filesystem::path &path) -> Result<DiskManager>;
  static auto OpenReadOnly(const std::filesystem::path &path) -> Result<DiskManager>;

  DiskManager(const DiskManager &) = delete;
  auto operator=(const DiskManager &) -> DiskManager & = delete;
  DiskManager(DiskManager &&) noexcept = default;
  auto operator=(DiskManager &&) -> DiskManager & = delete;
  ~DiskManager() = default;

  auto GetRootPageId() const -> page_id_t;
  auto GetAllocatorRootPageId() const -> page_id_t;
  auto HighWaterPageId() const -> page_id_t;
  auto TransactionId() const -> std::uint64_t;
  auto CheckpointLsn() const -> std::uint64_t;
  auto Uuid() const -> const DatabaseUuid &;

  // Checkpoint data-page writes may need to extend the file to the published
  // logical frontier. Allocation itself never performs this physical growth.
  auto EnsurePageCount(page_id_t high_water_page_id) -> Status;

  auto WriteCheckpointPage(page_id_t page_id, const char *data, page_id_t captured_high_water_page_id) const -> Status;
  auto CommitCheckpoint(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t high_water_page_id,
                        std::uint64_t transaction_id, std::uint64_t checkpoint_lsn) -> Status;
  auto Sync() const -> Status;

  // Copy exactly the pages owned by the selected durable superblock. The
  // caller supplies a new private file and is responsible for publishing its
  // name only after this method synchronizes the complete image.
  auto CopyCheckpointTo(int destination_fd) const -> Status;

  auto ReadPage(page_id_t page_id, char *data) const -> Status;

 private:
  static auto OpenImpl(const std::filesystem::path &path, bool writable) -> Result<DiskManager>;

  explicit DiskManager(UniqueFd fd, bool writable) : fd_(std::move(fd)), writable_(writable) {}

  auto EncodeSuperblock(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t high_water_page_id,
                        std::uint64_t transaction_id, std::uint64_t checkpoint_lsn,
                        std::uint64_t generation) const -> Result<std::array<char, PAGE_SIZE>>;
  auto EncodeCurrentSuperblock() const -> std::array<char, PAGE_SIZE>;

  UniqueFd fd_;
  bool writable_{true};

  // Logical metadata represented by the newest durable superblock. The
  // allocator index itself lives in ordinary checkpointed pages rooted here.
  DatabaseUuid database_uuid_{};
  std::uint64_t generation_{1};
  std::uint64_t checkpoint_lsn_{0};
  std::uint64_t transaction_id_{0};
  page_id_t root_page_id_{HEADER_PAGE_ID};
  page_id_t high_water_page_id_{FIRST_DATA_PAGE_ID};
  page_id_t allocator_root_page_id_{HEADER_PAGE_ID};
  page_id_t active_superblock_page_id_{SUPERBLOCK_A_PAGE_ID};
};

}  // namespace tinydb
