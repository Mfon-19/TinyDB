#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace tinydb {

/*
** DISK MANAGER BOUNDARY
**
** DiskManager owns the database file descriptor and the metadata represented
** by the currently published superblock. It performs physical page I/O and
** durable superblock selection; it does not allocate, retire, or reuse logical
** page IDs. Those operations belong to TransactionPages.
**
** Logical publication and checkpointing are separate. PrepareStateImage
** creates a redo-ready alternate superblock without changing this object's
** published fields. AdoptState changes those fields only after WAL durability
** and cache publication. Checkpoint writes the current state to the database
** file so that older WAL can eventually be discarded.
*/

/*
** A physical image suitable for redo. The bytes already contain their final
** identity and checksum, so recovery does not invoke tree or allocator logic.
*/
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

  auto GetRootPageId() const -> page_id_t;
  auto GetAllocatorRootPageId() const -> page_id_t;
  auto HighWaterPageId() const -> page_id_t;
  auto TransactionId() const -> std::uint64_t;
  auto CheckpointLsn() const -> std::uint64_t;
  auto Uuid() const -> const DatabaseUuid &;

  // Produces the alternate superblock image for a prepared transaction without
  // changing visible in-memory state. AdoptState is called only after that
  // image and the transaction's data pages become durable.
  auto PrepareStateImage(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t high_water_page_id,
                         std::uint64_t transaction_id, std::uint64_t checkpoint_lsn) const -> Result<PageImage>;
  void AdoptState(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t high_water_page_id,
                  std::uint64_t transaction_id, std::uint64_t checkpoint_lsn);
  void AdvanceCheckpoint(std::uint64_t checkpoint_lsn);

  // Checkpoint data-page writes may need to extend the file to the published
  // logical frontier. Allocation itself never performs this physical growth.
  auto EnsurePageCount(page_id_t high_water_page_id) -> Status;
  auto Checkpoint() -> Status;
  auto Sync() const -> Status;

  auto ReadPage(page_id_t page_id, char *data) const -> Status;
  auto WritePage(page_id_t page_id, const char *data) const -> Status;

 private:
  explicit DiskManager(UniqueFd fd) : fd_(std::move(fd)) {}

  auto EncodeSuperblock(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t high_water_page_id,
                        std::uint64_t transaction_id, std::uint64_t checkpoint_lsn,
                        std::uint64_t generation) const -> Result<std::array<char, PAGE_SIZE>>;
  auto EncodeCurrentSuperblock() const -> std::array<char, PAGE_SIZE>;
  auto WriteCurrentSuperblock() const -> Status;

  UniqueFd fd_;

  // Logical metadata represented by the latest published state. The allocator
  // index itself lives in ordinary committed pages rooted here.
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
