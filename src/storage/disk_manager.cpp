#include "storage/disk_manager.h"
#include "util/check.h"

#include "io/file_io.h"
#include "storage/superblock.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <random>
#include <span>
#include <utility>

namespace tinydb {
namespace {

using io::SyncParentDirectory;

/*
** Pages 0 and 1 are alternating superblocks.  A new database is created in
** this order:
**
**   reserve pages -> write A -> fsync file -> fsync parent directory
**                 -> write B -> fsync file
**
** Once the directory entry is durable, superblock A is already a complete
** root of database state and superblock B adds redundancy; a failure before A
** is written leaves a zero file that a later open can initialize again.
**
** During checkpoint, data pages are written but the WAL remains authoritative
** until those pages and a new inactive superblock have both been synchronized,
** regardless of whether the page-file transport is buffered or direct.
*/
auto RandomUuid() -> DatabaseUuid {
  auto uuid = DatabaseUuid{};
  auto random = std::random_device{};
  for (std::size_t offset = 0; offset < uuid.size(); offset += sizeof(std::uint32_t)) {
    const auto word = random();
    for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
      uuid[offset + byte] = static_cast<std::byte>((word >> (byte * 8U)) & 0xFFU);
    }
  }
  if (uuid == DatabaseUuid{}) {
    uuid.back() = std::byte{1};
  }
  return uuid;
}

auto ReadWholePage(const io::PageFile &file, page_id_t page_id) -> Result<storage::SuperblockPage> {
  auto page = storage::SuperblockPage{};
  if (auto status = file.ReadPage(page_id, page); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return page;
}

auto WriteWholePage(const io::PageFile &file, page_id_t page_id, const storage::SuperblockPage &page) -> Status {
  return file.WritePage(page_id, page);
}

}  // namespace
/*
** Open the database at path, creating and initializing it if it is empty.
** O_CREAT can leave an empty directory entry before initialization starts,
** while file growth can leave exactly two zero-filled superblock pages before
** the first superblock write; neither state contains an acknowledged database,
** so both are safe to initialize again.
*/
auto DiskManager::Open(const std::filesystem::path &path, PageIoMode mode) -> Result<DiskManager> {
  auto file = io::PageFile::Open(path, mode);
  if (!file) {
    return std::unexpected(file.error());
  }
  return Open(path, std::move(*file));
}

auto DiskManager::Open(const std::filesystem::path &path, io::PageFile file) -> Result<DiskManager> {
  struct stat file_stat {};
  if (auto status = file.Stat(&file_stat); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  DiskManager disk(std::move(file));

  const auto initialize_fresh = [&]() -> Status {
    disk.selected_.value.database_uuid = RandomUuid();
    if (auto status = disk.file_.EnsurePageCount(FIRST_DATA_PAGE_ID); !status.Ok()) {
      return status;
    }

    const auto initial = disk.EncodeCurrentSuperblock();
    if (auto status = WriteWholePage(disk.file_, SUPERBLOCK_A_PAGE_ID, initial); !status.Ok()) {
      return status;
    }
    if (auto status = disk.Sync(); !status.Ok()) {
      return status;
    }
    if (auto status = SyncParentDirectory(path); !status.Ok()) {
      return status;
    }
    if (auto status = WriteWholePage(disk.file_, SUPERBLOCK_B_PAGE_ID, initial); !status.Ok()) {
      return status;
    }
    if (auto status = disk.Sync(); !status.Ok()) {
      return status;
    }
    return {};
  };

  if (file_stat.st_size == 0) {
    if (auto status = initialize_fresh(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    return disk;
  }

  if (file_stat.st_size < static_cast<off_t>(FIRST_DATA_PAGE_ID * PAGE_SIZE) ||
      file_stat.st_size % static_cast<off_t>(PAGE_SIZE) != 0) {
    // Reject before mutation. In particular, do not silently truncate or
    // upgrade the former one-header format.
    return std::unexpected(Status::UnsupportedFormat("file does not contain TinyDB dual superblocks"));
  }

  const auto page_a = ReadWholePage(disk.file_, SUPERBLOCK_A_PAGE_ID);
  if (!page_a) {
    return std::unexpected(page_a.error());
  }
  const auto page_b = ReadWholePage(disk.file_, SUPERBLOCK_B_PAGE_ID);
  if (!page_b) {
    return std::unexpected(page_b.error());
  }
  const auto selected = storage::SelectSuperblock(*page_a, *page_b);
  if (!selected) {
    const auto page_is_zero = [](const storage::SuperblockPage &page) {
      return std::ranges::all_of(page, [](std::byte byte) { return byte == std::byte{0}; });
    };
    if (file_stat.st_size == static_cast<off_t>(FIRST_DATA_PAGE_ID * PAGE_SIZE) && page_is_zero(*page_a) &&
        page_is_zero(*page_b)) {
      // ftruncate() can reserve both pages before the first superblock write
      // fails. No UUID or database state was published, so creation can retry.
      if (auto status = initialize_fresh(); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
      return disk;
    }
    return std::unexpected(selected.error());
  }

  disk.selected_ = *selected;
  disk.durable_logical_page_count_.store(selected->value.logical_page_count, std::memory_order_relaxed);

  // A failed checkpoint can leave unreferenced pages beyond the logical end,
  // but a superblock can never claim pages beyond the physical file.
  const auto file_pages = static_cast<std::uint64_t>(file_stat.st_size) / PAGE_SIZE;
  if (disk.LogicalPageCount() > file_pages) {
    return std::unexpected(Status::Corruption("superblock logical page count exceeds the database file"));
  }
  return disk;
}

DiskManager::DiskManager(DiskManager &&other) noexcept
    : file_(std::move(other.file_)),
      selected_(other.selected_),
      durable_logical_page_count_(other.durable_logical_page_count_.load(std::memory_order_relaxed)) {}

auto DiskManager::GetRootPageId() const -> page_id_t { return selected_.value.root_page_id; }
auto DiskManager::GetAllocatorRootPageId() const -> page_id_t { return selected_.value.allocator_root_page_id; }
auto DiskManager::LogicalPageCount() const -> page_id_t {
  return durable_logical_page_count_.load(std::memory_order_acquire);
}
auto DiskManager::CheckpointLsn() const -> std::uint64_t { return selected_.value.checkpoint_lsn; }
auto DiskManager::Uuid() const -> const DatabaseUuid & { return selected_.value.database_uuid; }

auto DiskManager::EncodeCurrentSuperblock() const -> storage::SuperblockPage {
  const auto encoded = storage::EncodeSuperblock(selected_.value);
  TINYDB_CHECK(encoded.has_value(), "invalid durable superblock state");
  return *encoded;
}

auto DiskManager::EnsurePageCount(page_id_t logical_page_count) -> Status {
  if (logical_page_count < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("logical page count must include both superblocks");
  }
  return file_.EnsurePageCount(logical_page_count);
}

auto DiskManager::WriteCheckpointPages(page_id_t first_page_id, std::span<const std::byte *const> pages,
                                       page_id_t captured_logical_page_count) const -> Status {
  if (captured_logical_page_count < FIRST_DATA_PAGE_ID || first_page_id < FIRST_DATA_PAGE_ID || pages.empty() ||
      first_page_id >= captured_logical_page_count || pages.size() > captured_logical_page_count - first_page_id) {
    return Status::InvalidArgument("checkpoint write lies outside its captured logical page range");
  }
  return file_.WritePages(first_page_id, pages);
}

auto DiskManager::CommitCheckpoint(page_id_t root_page_id, page_id_t allocator_root_page_id,
                                   page_id_t logical_page_count, std::uint64_t checkpoint_lsn) -> Status {
  if (checkpoint_lsn < CheckpointLsn()) {
    return Status::InvalidArgument("checkpoint LSN moved backward");
  }
  if (selected_.value.generation == std::numeric_limits<std::uint64_t>::max()) {
    return Status::ResourceExhausted("superblock generation space exhausted");
  }

  auto next = selected_.value;
  ++next.generation;
  next.checkpoint_lsn = checkpoint_lsn;
  next.root_page_id = root_page_id;
  next.allocator_root_page_id = allocator_root_page_id;
  next.logical_page_count = logical_page_count;
  const auto encoded = storage::EncodeSuperblock(next);
  if (!encoded) {
    return encoded.error();
  }
  const auto next_slot =
      selected_.slot == storage::SuperblockSlot::A ? storage::SuperblockSlot::B : storage::SuperblockSlot::A;
  const auto inactive = next_slot == storage::SuperblockSlot::A ? SUPERBLOCK_A_PAGE_ID : SUPERBLOCK_B_PAGE_ID;
  if (auto status = WriteWholePage(file_, inactive, *encoded); !status.Ok()) {
    return status;
  }
  if (auto status = Sync(); !status.Ok()) {
    return status;
  }

  selected_ = storage::SelectedSuperblock{
      .value = next,
      .slot = next_slot,
  };
  // Direct read preparation can overlap checkpoint publication. Advance the
  // visible frontier only after its superblock is durable.
  durable_logical_page_count_.store(logical_page_count, std::memory_order_release);
  return {};
}

auto DiskManager::Sync() const -> Status { return file_.Sync(); }

auto DiskManager::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> data) const -> Status {
  // Ignore trailing writes from failed checkpoints. Only pages below the
  // selected superblock's frontier are readable.
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= LogicalPageCount()) {
    return Status::InvalidArgument("reading a page that was never allocated");
  }
  return file_.ReadPage(page_id, data);
}

auto DiskManager::UsesDirectIo() const noexcept -> bool { return file_.IsDirect(); }

auto DiskManager::BeginDirectReadPages(std::span<const page_id_t> page_ids,
                                       std::span<std::byte> contiguous_pages) const -> Result<io::DirectReadRequest> {
  // One frontier snapshot bounds the complete request. A concurrent checkpoint
  // can publish more pages, but it cannot change this validated range.
  const auto logical_page_count = LogicalPageCount();
  if (page_ids.empty() || std::ranges::any_of(page_ids, [logical_page_count](const auto page_id) {
        return page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count;
      })) {
    return std::unexpected(Status::InvalidArgument("asynchronous read lies outside the allocated database"));
  }
  return file_.BeginDirectReadPages(page_ids, contiguous_pages);
}

auto DiskManager::BeginDirectCheckpointWrite(page_id_t first_page_id, std::span<const struct iovec> vectors,
                                             page_id_t captured_logical_page_count) const
    -> Result<io::DirectWriteRequest> {
  // Data pages precede superblock publication, so this request uses the
  // captured checkpoint frontier rather than LogicalPageCount().
  if (captured_logical_page_count < FIRST_DATA_PAGE_ID || first_page_id < FIRST_DATA_PAGE_ID || vectors.empty() ||
      first_page_id >= captured_logical_page_count || vectors.size() > captured_logical_page_count - first_page_id) {
    return std::unexpected(Status::InvalidArgument("checkpoint write lies outside its captured logical page range"));
  }
  return file_.BeginDirectWritePages(first_page_id, vectors);
}

}  // namespace tinydb
