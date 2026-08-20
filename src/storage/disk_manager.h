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
** DiskManager owns the database PageFile and the metadata decoded from the
** selected superblock, which together form the durable view of the database.
** Newer state that exists only in the WAL or committed cache is not represented
** here; logical page allocation, retirement, and reuse belong to
** TransactionPages.
**
** Checkpoint data pages are written against a captured logical page count.
** CommitCheckpoint() writes the inactive superblock and synchronizes it before
** adopting the captured metadata in memory.  Until that synchronization
** succeeds, the old superblock and WAL remain the recovery basis, which makes
** a superblock a checkpoint artifact rather than a transaction page.
**
** PageFile selects buffered or direct I/O when the database is opened, and
** both transports implement the synchronous page and durability operations
** below; direct I/O additionally exposes prepared requests used by read-ahead
** and native checkpoint batches.
**
** selected_ remains writer-owned because most metadata is read only during
** open, commit, or checkpoint. The logical page frontier is copied into a
** release/acquire atomic because direct read preparation can overlap
** checkpoint publication. This avoids a mutex on every physical miss without
** exposing newly grown pages before their superblock is durable.
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

  // This grows the physical file for checkpoint; logical allocation has
  // already occurred.
  auto EnsurePageCount(page_id_t logical_page_count) -> Status;

  auto WriteCheckpointPages(page_id_t first_page_id, std::span<const std::byte *const> pages,
                            page_id_t captured_logical_page_count) const -> Status;

  auto CommitCheckpoint(page_id_t root_page_id, page_id_t allocator_root_page_id, page_id_t logical_page_count,
                        std::uint64_t checkpoint_lsn) -> Status;
  auto Sync() const -> Status;

  auto ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> data) const -> Status;
  auto UsesDirectIo() const noexcept -> bool;

  // These entry points are used only by the direct-I/O engine. Reads use the
  // durable page count; checkpoint writes use the count being published.
  auto BeginDirectReadPages(std::span<const page_id_t> page_ids,
                            std::span<std::byte> contiguous_pages) const -> Result<io::DirectReadRequest>;
  auto BeginDirectCheckpointWrite(page_id_t first_page_id, std::span<const struct iovec> vectors,
                                  page_id_t captured_logical_page_count) const -> Result<io::DirectWriteRequest>;

 private:
  explicit DiskManager(io::PageFile file) : file_(std::move(file)) {}

  auto EncodeCurrentSuperblock() const -> storage::SuperblockPage;

  io::PageFile file_;
  storage::SelectedSuperblock selected_{};

  // Direct-I/O reads can overlap checkpoint publication, so the reactor reads
  // this atomic copy instead of selected_. It advances only after fsync.
  std::atomic<page_id_t> durable_logical_page_count_{FIRST_DATA_PAGE_ID};
};

}  // namespace tinydb
