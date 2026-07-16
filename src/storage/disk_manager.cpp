#include <tinydb/check.h>
#include <tinydb/disk_manager.h>

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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

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
  const auto bytes_written = io::Pwrite(fd, page.data(), page.size(), page_id * PAGE_SIZE);
  if (bytes_written < 0) {
    return ErrnoStatus("pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != page.size()) {
    return Status::IoError("short write on a persistent page");
  }
  return {};
}

auto ToBytes(const std::array<char, PAGE_SIZE> &page) -> std::span<const std::byte> {
  return std::as_bytes(std::span{page});
}

}  // namespace

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
    if (auto status = initialize_fresh(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    return disk;
  }

  if (file_stat.st_size < static_cast<off_t>(FIRST_DATA_PAGE_ID * PAGE_SIZE) ||
      file_stat.st_size % static_cast<off_t>(PAGE_SIZE) != 0) {
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
      if (auto status = initialize_fresh(); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
      return disk;
    }
    return std::unexpected(selected.error());
  }

  disk.database_uuid_ = selected->value.database_uuid;
  disk.generation_ = selected->value.generation;
  disk.checkpoint_lsn_ = selected->value.checkpoint_lsn;
  disk.transaction_id_ = selected->value.transaction_id;
  disk.root_page_id_ = selected->value.root_page_id;
  disk.next_page_id_ = selected->value.high_water_page_id;
  disk.free_list_head_ = selected->value.allocator_root_page_id;
  disk.active_superblock_page_id_ =
      selected->slot == storage::SuperblockSlot::A ? SUPERBLOCK_A_PAGE_ID : SUPERBLOCK_B_PAGE_ID;

  const auto file_pages = static_cast<std::uint64_t>(file_stat.st_size) / PAGE_SIZE;
  if (disk.next_page_id_ < FIRST_DATA_PAGE_ID || disk.next_page_id_ > file_pages) {
    return std::unexpected(Status::Corruption("superblock allocation frontier lies outside the database file"));
  }

  auto free_page_id = disk.free_list_head_;
  while (free_page_id != HEADER_PAGE_ID) {
    if (free_page_id < FIRST_DATA_PAGE_ID || free_page_id >= disk.next_page_id_ ||
        disk.free_pages_.contains(free_page_id)) {
      return std::unexpected(Status::Corruption("corrupt allocator free list"));
    }
    const auto page = ReadWholePage(disk.fd_.Get(), free_page_id);
    if (!page) {
      return std::unexpected(page.error());
    }
    const auto decoded = storage::DecodeAllocatorPage(ToBytes(*page), free_page_id);
    if (!decoded) {
      return std::unexpected(decoded.error());
    }
    disk.free_pages_.insert(free_page_id);
    free_page_id = decoded->next_free;
  }
  return disk;
}

auto DiskManager::AllocatePage() -> Result<page_id_t> {
  TINYDB_CHECK(fd_.Valid(), "allocating a page on a closed disk manager");

  if (free_list_head_ != HEADER_PAGE_ID) {
    const auto page_id = free_list_head_;
    auto next_free = HEADER_PAGE_ID;
    if (const auto pending = pending_free_links_.find(page_id); pending != pending_free_links_.end()) {
      next_free = pending->second;
      pending_free_links_.erase(pending);
      std::erase(op_freed_pages_, page_id);
    } else {
      const auto page = ReadWholePage(fd_.Get(), page_id);
      if (!page) {
        return std::unexpected(page.error());
      }
      const auto decoded = storage::DecodeAllocatorPage(ToBytes(*page), page_id);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      next_free = decoded->next_free;
    }
    free_list_head_ = next_free;
    free_pages_.erase(page_id);
    header_changed_ = true;
    return page_id;
  }

  const auto page_id = next_page_id_;
  if (io::Ftruncate(fd_.Get(), (page_id + 1) * PAGE_SIZE) < 0) {
    return std::unexpected(ErrnoStatus("ftruncate"));
  }
  ++next_page_id_;
  header_changed_ = true;
  return page_id;
}

void DiskManager::FreePage(page_id_t page_id) {
  TINYDB_CHECK(fd_.Valid(), "freeing a page on a closed disk manager");
  TINYDB_CHECK(page_id >= FIRST_DATA_PAGE_ID && page_id < next_page_id_, "freeing a page that was never allocated");
  TINYDB_CHECK(!free_pages_.contains(page_id), "double free of a page");

  pending_free_links_[page_id] = free_list_head_;
  op_freed_pages_.push_back(page_id);
  free_pages_.insert(page_id);
  free_list_head_ = page_id;
  header_changed_ = true;
}

auto DiskManager::GetRootPageId() const -> page_id_t { return root_page_id_; }
auto DiskManager::NextPageId() const -> page_id_t { return next_page_id_; }
auto DiskManager::FreePages() const -> const std::unordered_set<page_id_t> & { return free_pages_; }
auto DiskManager::Uuid() const -> const DatabaseUuid & { return database_uuid_; }

void DiskManager::SetRootPageId(page_id_t root_page_id) {
  TINYDB_CHECK(root_page_id == HEADER_PAGE_ID || (root_page_id >= FIRST_DATA_PAGE_ID && root_page_id < next_page_id_),
               "root page is outside the allocation frontier");
  root_page_id_ = root_page_id;
  header_changed_ = true;
}

auto DiskManager::EncodeCurrentSuperblock() const -> std::array<char, PAGE_SIZE> {
  const auto encoded = storage::EncodeSuperblock(storage::Superblock{
      .database_uuid = database_uuid_,
      .generation = generation_,
      .checkpoint_lsn = checkpoint_lsn_,
      .transaction_id = transaction_id_,
      .root_page_id = root_page_id_,
      .allocator_root_page_id = free_list_head_,
      .high_water_page_id = next_page_id_,
  });
  TINYDB_CHECK(encoded.has_value(), "invalid in-memory superblock state");
  auto output = std::array<char, PAGE_SIZE>{};
  std::memcpy(output.data(), encoded->data(), encoded->size());
  return output;
}

auto DiskManager::AdvanceSuperblock() -> page_id_t {
  ++generation_;
  ++transaction_id_;
  active_superblock_page_id_ =
      active_superblock_page_id_ == SUPERBLOCK_A_PAGE_ID ? SUPERBLOCK_B_PAGE_ID : SUPERBLOCK_A_PAGE_ID;
  return active_superblock_page_id_;
}

auto DiskManager::TakeOpImages() -> std::vector<PageImage> {
  auto images = std::vector<PageImage>{};
  if (header_changed_) {
    const auto page_id = AdvanceSuperblock();
    images.push_back(PageImage{.page_id = page_id, .data = EncodeCurrentSuperblock()});
    header_changed_ = false;
  }

  for (const auto page_id : op_freed_pages_) {
    const auto link = pending_free_links_.find(page_id);
    TINYDB_CHECK(link != pending_free_links_.end(), "freed page has no pending allocator link");
    const auto encoded = storage::EncodeAllocatorPage(page_id, transaction_id_, link->second);
    TINYDB_CHECK(encoded.has_value(), "invalid in-memory allocator page");
    images.push_back(PageImage{.page_id = page_id, .data = *encoded});
  }
  op_freed_pages_.clear();
  return images;
}

auto DiskManager::Checkpoint() -> Status {
  TINYDB_CHECK(fd_.Valid(), "checkpointing a closed disk manager");
  op_freed_pages_.clear();

  for (auto link = pending_free_links_.begin(); link != pending_free_links_.end();) {
    const auto encoded = storage::EncodeAllocatorPage(link->first, transaction_id_, link->second);
    if (!encoded) {
      return encoded.error();
    }
    if (auto status = WriteWholePage(fd_.Get(), link->first, *encoded); !status.Ok()) {
      return status;
    }
    link = pending_free_links_.erase(link);
  }

  if (header_changed_) {
    AdvanceSuperblock();
    header_changed_ = false;
  }
  return WriteCurrentSuperblock();
}

auto DiskManager::WriteCurrentSuperblock() const -> Status {
  const auto encoded = EncodeCurrentSuperblock();
  if (auto status = WriteWholePage(fd_.Get(), active_superblock_page_id_, encoded); !status.Ok()) {
    return status;
  }
  const auto mirror = active_superblock_page_id_ == SUPERBLOCK_A_PAGE_ID ? SUPERBLOCK_B_PAGE_ID : SUPERBLOCK_A_PAGE_ID;
  return WriteWholePage(fd_.Get(), mirror, encoded);
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
  TINYDB_CHECK(!free_pages_.contains(page_id), "reading a freed page");
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= next_page_id_) {
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

auto DiskManager::WritePage(page_id_t page_id, const char *data) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "writing to a closed disk manager");
  TINYDB_CHECK(!free_pages_.contains(page_id), "writing to a freed page");
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= next_page_id_) {
    return Status::InvalidArgument("writing a page that was never allocated");
  }
  const auto bytes_written = io::Pwrite(fd_.Get(), data, PAGE_SIZE, page_id * PAGE_SIZE);
  if (bytes_written < 0) {
    return ErrnoStatus("pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != PAGE_SIZE) {
    return Status::IoError("short write on a page");
  }
  return {};
}

}  // namespace tinydb
