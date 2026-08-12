#pragma once

#include <tinydb/options.h>
#include <tinydb/status.h>
#include "io/page_file.h"
#include "storage/page.h"
#include "storage/superblock.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

namespace tinydb {

/*
** DISK MANAGER BOUNDARY
**
** DiskManager owns the database file descriptor and the metadata represented
** by the newest durable superblock. It performs physical page I/O and durable
** superblock selection. It does not track the newer state visible only through
** the committed cache and WAL. Logical allocation, retirement, and reuse
** belong to TransactionPages.
**
** Checkpoint data pages are written against a captured logical page count.
** CommitCheckpoint then writes only the inactive superblock and
** synchronizes it before adopting the captured metadata in memory. Until that
** synchronization succeeds, the previously selected superblock and WAL remain
** the recovery basis. Superblocks are checkpoint artifacts. They are never
** transaction page images.
**
** PageFile keeps the selected buffered or direct transport. Synchronous page
** operations work in both modes. Native requests are optional direct-mode
** acceleration.
*/

class DiskManager {
 public:
  static auto Open(const std::filesystem::path &path, PageIoMode mode = PageIoMode::Buffered) -> Result<DiskManager>;
  static auto Open(const std::filesystem::path &path, io::PageFile file) -> Result<DiskManager>;

  DiskManager(const DiskManager &) = delete;
  auto operator=(const DiskManager &) -> DiskManager & = delete;
  DiskManager(DiskManager &&other) noexcept;
  auto operator=(DiskManager &&) -> DiskManager & = delete;
  ~DiskManager() = default;

  auto GetRootPageId() const -> page_id_t;
  auto GetAllocatorRootPageId() const -> page_id_t;
  auto LogicalPageCount() const -> page_id_t;
  auto CheckpointLsn() const -> std::uint64_t;
  auto Uuid() const -> const DatabaseUuid &;

  // Checkpoint data-page writes can extend the file to the committed logical
  // page count. Allocation itself never performs this physical growth.
  auto EnsurePageCount(page_id_t logical_page_count) -> Status;

  auto WriteCheckpointPage(page_id_t page_id, const char *data, page_id_t captured_logical_page_count) const -> Status;
  auto WriteCheckpointPages(page_id_t first_page_id, std::span<const std::byte *const> pages,
                            page_id_t captured_logical_page_count) const -> Status;
  auto CommitCheckpoint(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t logical_page_count,
                        std::uint64_t checkpoint_lsn) -> Status;
  auto Sync() const -> Status;

  auto ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> data) const -> Status;
  auto UsesDirectIo() const noexcept -> bool;
  // Direct reads use the current durable page frontier. Direct checkpoint
  // writes use the captured frontier that the checkpoint will publish.
  auto BeginDirectReadPages(std::span<const page_id_t> page_ids,
                            std::span<std::byte> contiguous_pages) const -> Result<io::DirectReadRequest>;
  auto BeginDirectCheckpointWrite(page_id_t first_page_id, std::span<const struct iovec> vectors,
                                  page_id_t captured_logical_page_count) const -> Result<io::DirectWriteRequest>;

 private:
  explicit DiskManager(io::PageFile file) : file_(std::move(file)) {}

  auto EncodeCurrentSuperblock() const -> storage::SuperblockPage;

  io::PageFile file_;
  storage::SelectedSuperblock selected_{};
  // Native reactor threads read this frontier without access to selected_. A
  // successful superblock synchronization publishes the new value.
  std::atomic<page_id_t> durable_logical_page_count_{FIRST_DATA_PAGE_ID};
};

}  // namespace tinydb
