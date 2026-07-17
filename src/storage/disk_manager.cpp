#include "storage/disk_manager.h"
#include "util/check.h"

#include "io/file_io.h"
#include "io/syscalls.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tinydb {
namespace {

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
auto ErrnoStatus(std::string_view operation) -> Status {
  // Capture errno immediately at the syscall boundary; later library work can
  // overwrite the thread-local value and produce a misleading status.
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

// fsync(file) makes file contents durable, but creation also changes the
// parent directory. Without this second fsync a power loss can forget the name
// even though both superblock writes reached stable storage.
auto SyncParentDirectory(const std::filesystem::path &path) -> Status {
  auto parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  auto directory = UniqueFd(io::Open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory.Valid()) {
    return ErrnoStatus("open directory");
  }
  if (io::Fsync(directory.Get()) < 0) {
    return ErrnoStatus("fsync directory");
  }
  return {};
}

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
  if (std::ranges::all_of(uuid, [](std::byte value) { return value == std::byte{0}; })) {
    uuid.back() = std::byte{1};
  }
  return uuid;
}

auto ReadWholePage(int fd, page_id_t page_id) -> Result<std::array<char, PAGE_SIZE>> {
  // Persistent pages are atomic units at the storage abstraction even though
  // POSIX may return a short read. A short page cannot be safely decoded.
  auto page = std::array<char, PAGE_SIZE>{};
  const auto bytes_read = io::Pread(fd, page.data(), page.size(), page_id * PAGE_SIZE);
  if (bytes_read < 0) {
    return std::unexpected(ErrnoStatus("pread"));
  }
  if (static_cast<std::size_t>(bytes_read) != page.size()) {
    return std::unexpected(Status::Corruption("short read on a persistent page"));
  }
  return page;
}

auto WriteWholePage(int fd, page_id_t page_id, const std::array<char, PAGE_SIZE> &page) -> Status {
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
** files pass superblock and physical-frontier validation before any mutation.
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
    disk.database_uuid_ = RandomUuid();
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
  disk.database_uuid_ = selected->value.database_uuid;
  disk.generation_ = selected->value.generation;
  disk.checkpoint_lsn_ = selected->value.checkpoint_lsn;
  disk.transaction_id_ = selected->value.transaction_id;
  disk.root_page_id_ = selected->value.root_page_id;
  disk.high_water_page_id_ = selected->value.high_water_page_id;
  disk.allocator_root_page_id_ = selected->value.allocator_root_page_id;
  disk.active_superblock_page_id_ =
      selected->slot == storage::SuperblockSlot::A ? SUPERBLOCK_A_PAGE_ID : SUPERBLOCK_B_PAGE_ID;

  // The superblock may not claim pages beyond the physical file. It may claim
  // fewer pages because a failed checkpoint can leave harmless trailing data.
  const auto file_pages = static_cast<std::uint64_t>(file_stat.st_size) / PAGE_SIZE;
  if (disk.high_water_page_id_ < FIRST_DATA_PAGE_ID || disk.high_water_page_id_ > file_pages) {
    return std::unexpected(Status::Corruption("superblock allocation frontier lies outside the database file"));
  }
  return disk;
}

auto DiskManager::GetRootPageId() const -> page_id_t { return root_page_id_; }
auto DiskManager::GetAllocatorRootPageId() const -> page_id_t { return allocator_root_page_id_; }
auto DiskManager::HighWaterPageId() const -> page_id_t { return high_water_page_id_; }
auto DiskManager::TransactionId() const -> std::uint64_t { return transaction_id_; }
auto DiskManager::CheckpointLsn() const -> std::uint64_t { return checkpoint_lsn_; }
auto DiskManager::Uuid() const -> const DatabaseUuid & { return database_uuid_; }

auto DiskManager::EncodeSuperblock(page_id_t root_page_id, page_id_t allocator_root_page_id,
                                   page_id_t high_water_page_id, std::uint64_t transaction_id,
                                   std::uint64_t checkpoint_lsn,
                                   std::uint64_t generation) const -> Result<std::array<char, PAGE_SIZE>> {
  const auto encoded = storage::EncodeSuperblock(storage::Superblock{
      .database_uuid = database_uuid_,
      .generation = generation,
      .checkpoint_lsn = checkpoint_lsn,
      .transaction_id = transaction_id,
      .root_page_id = root_page_id,
      .allocator_root_page_id = allocator_root_page_id,
      .high_water_page_id = high_water_page_id,
  });
  if (!encoded) {
    return std::unexpected(encoded.error());
  }
  auto output = std::array<char, PAGE_SIZE>{};
  std::memcpy(output.data(), encoded->data(), encoded->size());
  return output;
}

auto DiskManager::EncodeCurrentSuperblock() const -> std::array<char, PAGE_SIZE> {
  const auto encoded = EncodeSuperblock(root_page_id_, allocator_root_page_id_, high_water_page_id_, transaction_id_,
                                        checkpoint_lsn_, generation_);
  TINYDB_CHECK(encoded.has_value(), "invalid durable superblock state");
  return *encoded;
}

auto DiskManager::EnsurePageCount(page_id_t high_water_page_id) -> Status {
  TINYDB_CHECK(fd_.Valid(), "extending a closed disk manager");
  if (high_water_page_id < FIRST_DATA_PAGE_ID) {
    return Status::InvalidArgument("allocation frontier overlaps superblocks");
  }
  // File growth is physical preparation for checkpoint, not page allocation.
  // The logical frontier was already committed before this call.
  if (io::Ftruncate(fd_.Get(), high_water_page_id * PAGE_SIZE) < 0) {
    return ErrnoStatus("ftruncate");
  }
  return {};
}

auto DiskManager::WriteCheckpointPage(page_id_t page_id, const char *data,
                                      page_id_t captured_high_water_page_id) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "writing through a closed disk manager");
  if (data == nullptr || captured_high_water_page_id < FIRST_DATA_PAGE_ID || page_id < FIRST_DATA_PAGE_ID ||
      page_id >= captured_high_water_page_id) {
    return Status::InvalidArgument("checkpoint page lies outside its captured allocation frontier");
  }
  return io::FullPwrite(fd_.Get(), data, PAGE_SIZE, page_id * PAGE_SIZE);
}

auto DiskManager::CommitCheckpoint(page_id_t root_page_id, page_id_t allocator_root_page_id,
                                   page_id_t high_water_page_id, std::uint64_t transaction_id,
                                   std::uint64_t checkpoint_lsn) -> Status {
  TINYDB_CHECK(fd_.Valid(), "checkpointing a closed disk manager");
  if (checkpoint_lsn < checkpoint_lsn_) {
    return Status::InvalidArgument("checkpoint frontier moved backward");
  }
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return Status::ResourceExhausted("superblock generation space exhausted");
  }

  /*
  ** SUPERBLOCK PUBLICATION
  **
  ** Data pages were synchronized by CheckpointManager before this call. Write
  ** the slot not named by active_superblock_page_id_, then synchronize that
  ** one new recovery root. The old slot is deliberately left untouched.
  ** In-memory metadata changes only after fsync succeeds.
  */
  const auto next_generation = generation_ + 1U;
  const auto encoded = EncodeSuperblock(root_page_id, allocator_root_page_id, high_water_page_id, transaction_id,
                                        checkpoint_lsn, next_generation);
  if (!encoded) {
    return encoded.error();
  }
  const auto inactive =
      active_superblock_page_id_ == SUPERBLOCK_A_PAGE_ID ? SUPERBLOCK_B_PAGE_ID : SUPERBLOCK_A_PAGE_ID;
  if (auto status = WriteWholePage(fd_.Get(), inactive, *encoded); !status.Ok()) {
    return status;
  }
  if (auto status = Sync(); !status.Ok()) {
    return status;
  }

  generation_ = next_generation;
  checkpoint_lsn_ = checkpoint_lsn;
  transaction_id_ = transaction_id;
  root_page_id_ = root_page_id;
  allocator_root_page_id_ = allocator_root_page_id;
  high_water_page_id_ = high_water_page_id;
  active_superblock_page_id_ = inactive;
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
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= high_water_page_id_) {
    // Invalid requests are programming/API errors; short or malformed bytes
    // inside an allocated page are persistent corruption.
    return Status::InvalidArgument("reading a page that was never allocated");
  }
  const auto bytes_read = io::Pread(fd_.Get(), data, PAGE_SIZE, page_id * PAGE_SIZE);
  if (bytes_read < 0) {
    return ErrnoStatus("pread");
  }
  if (static_cast<std::size_t>(bytes_read) != PAGE_SIZE) {
    return Status::Corruption("short read on a page");
  }
  return {};
}

}  // namespace tinydb
