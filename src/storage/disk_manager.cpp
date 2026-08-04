#include "storage/disk_manager.h"
#include "util/check.h"

#include "io/file_io.h"
#include "io/testable_posix.h"
#include "storage/superblock.h"

#include <fcntl.h>
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

using io::ErrnoStatus;
using io::SyncParentDirectory;

/*
** DATABASE FILE DURABILITY
**
** Pages 0 and 1 are alternating superblocks. Opening selects the valid copy
** with the greatest generation. A fresh file is created in this order:
**
**   reserve pages -> write A -> fsync file -> fsync parent directory
**                 -> write B -> fsync file
**
** Thus any completed creation has at least one durable, checksummed root of
** state, and a crash before completion leaves either a retryable zero file or
** a valid first copy. Data-page writes occur during checkpoint. The WAL stays
** authoritative until those writes and the next inactive superblock are
** synchronized.
*/
auto RandomUuid() -> DatabaseUuid {
  // std::random_device is used only for collision-resistant identity, not for
  // cryptographic authorization. A UUID collision could pair unrelated WAL
  // and database files, so still reject the one reserved all-zero result.
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

auto ReadWholePage(int fd, page_id_t page_id) -> Result<std::array<char, PAGE_SIZE>> {
  // Persistent pages are atomic units at the storage abstraction even though
  // POSIX may return a short read. A short page cannot be safely decoded.
  auto page = std::array<char, PAGE_SIZE>{};
  const auto bytes_read = io::FullPread(fd, page.data(), page.size(), page_id * PAGE_SIZE);
  if (!bytes_read) {
    return std::unexpected(bytes_read.error());
  }
  if (*bytes_read != page.size()) {
    return std::unexpected(Status::Corruption("short read on a persistent page"));
  }
  return page;
}

auto WriteWholePage(int fd, page_id_t page_id, const storage::SuperblockPage &page) -> Status {
  // Checkpoint and superblock pages are logical 4 KiB writes even when POSIX
  // completes them through several short writes or an interrupted syscall.
  return io::FullPwrite(fd, page.data(), page.size(), page_id * PAGE_SIZE);
}

auto ToBytes(const std::array<char, PAGE_SIZE> &page) -> std::span<const std::byte> {
  return std::as_bytes(std::span{page});
}

}  // namespace

/*
** Open may create and initialize an absent database. O_CREAT can leave an
** empty directory entry before initialization begins, so the zero-file and
** two-zero-superblock states are explicitly retryable. Existing nonempty
** files pass superblock and file-size validation before any mutation.
*/
auto DiskManager::Open(const std::filesystem::path &path) -> Result<DiskManager> {
  auto fd = UniqueFd(io::Open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }

  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  DiskManager disk(std::move(fd));

  const auto initialize_fresh = [&]() -> Status {
    // Creation ordering:
    //   1. reserve both metadata pages;
    //   2. write and fsync A, establishing one recoverable copy;
    //   3. fsync the directory entry;
    //   4. write and fsync B, establishing redundancy.
    // At every boundary either no valid database exists yet or at least one
    // complete superblock does.
    disk.selected_.value.database_uuid = RandomUuid();
    if (io::Ftruncate(disk.fd_.Get(), FIRST_DATA_PAGE_ID * PAGE_SIZE) < 0) {
      return ErrnoStatus("ftruncate");
    }

    const auto initial = disk.EncodeCurrentSuperblock();
    if (auto status = WriteWholePage(disk.fd_.Get(), SUPERBLOCK_A_PAGE_ID, initial); !status.Ok()) {
      return status;
    }
    if (auto status = disk.Sync(); !status.Ok()) {
      return status;
    }
    if (auto status = SyncParentDirectory(path); !status.Ok()) {
      return status;
    }
    if (auto status = WriteWholePage(disk.fd_.Get(), SUPERBLOCK_B_PAGE_ID, initial); !status.Ok()) {
      return status;
    }
    if (auto status = disk.Sync(); !status.Ok()) {
      return status;
    }
    return {};
  };

  if (file_stat.st_size == 0) {
    // Includes a zero-byte entry left by a failed/terminated earlier creation.
    if (auto status = initialize_fresh(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    return disk;
  }

  if (file_stat.st_size < static_cast<off_t>(FIRST_DATA_PAGE_ID * PAGE_SIZE) ||
      file_stat.st_size % static_cast<off_t>(PAGE_SIZE) != 0) {
    // Reject before mutation. In particular, the former one-header format is
    // not silently upgraded or truncated into the new dual-superblock format.
    return std::unexpected(Status::UnsupportedFormat("file does not contain TinyDB dual superblocks"));
  }

  const auto page_a = ReadWholePage(disk.fd_.Get(), SUPERBLOCK_A_PAGE_ID);
  if (!page_a) {
    return std::unexpected(page_a.error());
  }
  const auto page_b = ReadWholePage(disk.fd_.Get(), SUPERBLOCK_B_PAGE_ID);
  if (!page_b) {
    return std::unexpected(page_b.error());
  }
  const auto selected = storage::SelectSuperblock(ToBytes(*page_a), ToBytes(*page_b));
  if (!selected) {
    const auto page_is_zero = [](const std::array<char, PAGE_SIZE> &page) {
      return std::ranges::all_of(page, [](char byte) { return byte == 0; });
    };
    if (file_stat.st_size == static_cast<off_t>(FIRST_DATA_PAGE_ID * PAGE_SIZE) && page_is_zero(*page_a) &&
        page_is_zero(*page_b)) {
      // ftruncate may have reserved two zero-filled pages before the first
      // superblock write failed. This exact state contains no acknowledged
      // database, so creation can safely start again with a new UUID.
      if (auto status = initialize_fresh(); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
      return disk;
    }
    return std::unexpected(selected.error());
  }

  // Adopt the selected state only after both pages have been independently
  // decoded. No persistent mutation occurs on the existing-file open path.
  disk.selected_ = *selected;

  // The superblock may not claim pages beyond the physical file. It may claim
  // fewer pages because a failed checkpoint can leave harmless trailing data.
  const auto file_pages = static_cast<std::uint64_t>(file_stat.st_size) / PAGE_SIZE;
  if (disk.LogicalPageCount() > file_pages) {
    return std::unexpected(Status::Corruption("superblock logical page count exceeds the database file"));
  }
  return disk;
}

auto DiskManager::GetRootPageId() const -> page_id_t { return selected_.value.root_page_id; }
auto DiskManager::GetAllocatorRootPageId() const -> page_id_t { return selected_.value.allocator_root_page_id; }
auto DiskManager::LogicalPageCount() const -> page_id_t { return selected_.value.logical_page_count; }
auto DiskManager::CheckpointLsn() const -> std::uint64_t { return selected_.value.checkpoint_lsn; }
auto DiskManager::Uuid() const -> const DatabaseUuid & { return selected_.value.database_uuid; }

auto DiskManager::EncodeCurrentSuperblock() const -> storage::SuperblockPage {
  const auto encoded = storage::EncodeSuperblock(selected_.value);
  TINYDB_CHECK(encoded.has_value(), "invalid durable superblock state");
  return *encoded;
}

auto DiskManager::EnsurePageCount(page_id_t logical_page_count) -> Status {
  TINYDB_CHECK(fd_.Valid(), "extending a closed disk manager");
  if (logical_page_count < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("logical page count must include both superblocks");
  }
  // File growth is physical preparation for checkpoint, not page allocation.
  // The logical page count was already committed before this call.
  if (io::Ftruncate(fd_.Get(), logical_page_count * PAGE_SIZE) < 0) {
    return ErrnoStatus("ftruncate");
  }
  return {};
}

auto DiskManager::WriteCheckpointPage(page_id_t page_id, const char *data,
                                      page_id_t captured_logical_page_count) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "writing through a closed disk manager");
  if (data == nullptr || captured_logical_page_count < FIRST_DATA_PAGE_ID || page_id < FIRST_DATA_PAGE_ID ||
      page_id >= captured_logical_page_count) {
    return Status::InvalidArgument("checkpoint page lies outside its captured logical page range");
  }
  return io::FullPwrite(fd_.Get(), data, PAGE_SIZE, page_id * PAGE_SIZE);
}

auto DiskManager::CommitCheckpoint(page_id_t root_page_id, page_id_t allocator_root_page_id,
                                   page_id_t logical_page_count, std::uint64_t checkpoint_lsn) -> Status {
  TINYDB_CHECK(fd_.Valid(), "checkpointing a closed disk manager");
  if (checkpoint_lsn < CheckpointLsn()) {
    return Status::InvalidArgument("checkpoint LSN moved backward");
  }
  if (selected_.value.generation == std::numeric_limits<std::uint64_t>::max()) {
    return Status::ResourceExhausted("superblock generation space exhausted");
  }

  /*
  ** SUPERBLOCK PUBLICATION
  **
  ** Data pages were synchronized by CheckpointManager before this call. Write
  ** the inactive slot, then synchronize that one new recovery root. The old
  ** slot is left untouched.
  ** In-memory metadata changes only after fsync succeeds.
  */
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
  if (auto status = WriteWholePage(fd_.Get(), inactive, *encoded); !status.Ok()) {
    return status;
  }
  if (auto status = Sync(); !status.Ok()) {
    return status;
  }

  selected_ = storage::SelectedSuperblock{
      .value = next,
      .slot = next_slot,
  };
  return {};
}

auto DiskManager::Sync() const -> Status {
  TINYDB_CHECK(fd_.Valid(), "syncing a closed disk manager");
  if (io::Fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  return {};
}

auto DiskManager::ReadPage(page_id_t page_id, char *data) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "reading from a closed disk manager");
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= LogicalPageCount()) {
    return Status::InvalidArgument("reading a page that was never allocated");
  }
  const auto bytes_read = io::FullPread(fd_.Get(), data, PAGE_SIZE, page_id * PAGE_SIZE);
  if (!bytes_read) {
    return bytes_read.error();
  }
  if (*bytes_read != PAGE_SIZE) {
    return Status::Corruption("short read on a page");
  }
  return {};
}

}  // namespace tinydb
