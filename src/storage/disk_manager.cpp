#include <tinydb/check.h>
#include <tinydb/disk_manager.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tinydb {
static constexpr std::uint32_t FILE_MAGIC = 0x54444231U;

namespace {

// The failing errno as an IoError status, tagged with the operation.
auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

// Reads just the free-list header at the front of a free page.
auto ReadFreePageHeader(int fd, page_id_t page_id) -> Result<FreePageHeader> {
  FreePageHeader header{};
  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_read = ::pread(fd, &header, sizeof(header), offset);
  if (bytes_read < 0) {
    return std::unexpected(ErrnoStatus("pread"));
  }
  if (static_cast<std::size_t>(bytes_read) != sizeof(header)) {
    return std::unexpected(Status::Corruption("short read on a free page header"));
  }
  return header;
}

}  // namespace

auto DiskManager::Open(const std::filesystem::path &path) -> Result<DiskManager> {
  auto fd = UniqueFd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }

  struct stat stat_buffer {};
  if (::fstat(fd.Get(), &stat_buffer) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }

  DiskManager disk(std::move(fd));

  if (stat_buffer.st_size == 0) {
    disk.header_ = FileHeader{
        .magic = FILE_MAGIC,
        .page_size = PAGE_SIZE,
        .root_page_id = HEADER_PAGE_ID,
        .next_page_id = FIRST_DATA_PAGE_ID,
        .free_list_head = HEADER_PAGE_ID,
    };

    if (auto status = disk.WriteHeader(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    return disk;
  }

  const auto bytes_read = ::pread(disk.fd_.Get(), &disk.header_, sizeof(disk.header_), 0);
  if (bytes_read < 0) {
    return std::unexpected(ErrnoStatus("pread"));
  }

  // A file too short to hold a header cannot be a database either.
  if (static_cast<std::size_t>(bytes_read) != sizeof(disk.header_) || disk.header_.magic != FILE_MAGIC ||
      disk.header_.page_size != PAGE_SIZE) {
    return std::unexpected(Status::InvalidArgument("not a TinyDB database file: " + path.string()));
  }

  // Rebuild the in-memory free-page set. Walking the list here also proves
  // it is acyclic and points only at allocated pages marked free.
  auto free_page_id = disk.header_.free_list_head;
  while (free_page_id != HEADER_PAGE_ID) {
    if (free_page_id >= disk.header_.next_page_id || disk.free_pages_.contains(free_page_id)) {
      return std::unexpected(Status::Corruption("corrupt free list in " + path.string()));
    }
    const auto free_header = ReadFreePageHeader(disk.fd_.Get(), free_page_id);
    if (!free_header) {
      return std::unexpected(free_header.error());
    }
    if (free_header->type != FREE_PAGE_TYPE) {
      return std::unexpected(Status::Corruption("corrupt free list in " + path.string()));
    }
    disk.free_pages_.insert(free_page_id);
    free_page_id = free_header->next_free;
  }
  return disk;
}

auto DiskManager::AllocatePage() -> Result<page_id_t> {
  TINYDB_CHECK(fd_.Valid(), "allocating a page on a closed disk manager");

  // Pop the most recently freed page when one is available.
  if (header_.free_list_head != HEADER_PAGE_ID) {
    const auto page_id = header_.free_list_head;
    const auto free_header = ReadFreePageHeader(fd_.Get(), page_id);
    if (!free_header) {
      return std::unexpected(free_header.error());
    }
    TINYDB_CHECK(free_header->type == FREE_PAGE_TYPE, "free list head is not a free page");

    header_.free_list_head = free_header->next_free;
    free_pages_.erase(page_id);
    if (auto status = WriteHeader(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    return page_id;
  }

  // Otherwise grow the file by one page.
  const auto page_id = header_.next_page_id;
  const auto new_size = static_cast<off_t>((page_id + 1) * PAGE_SIZE);

  if (::ftruncate(fd_.Get(), new_size) < 0) {
    return std::unexpected(ErrnoStatus("ftruncate"));
  }

  ++header_.next_page_id;
  if (auto status = WriteHeader(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  return page_id;
}

auto DiskManager::FreePage(page_id_t page_id) -> Status {
  TINYDB_CHECK(fd_.Valid(), "freeing a page on a closed disk manager");
  const bool allocated = page_id != HEADER_PAGE_ID && page_id < header_.next_page_id;
  TINYDB_CHECK(allocated, "freeing a page that was never allocated");
  TINYDB_CHECK(!free_pages_.contains(page_id), "double free of a page");

  const auto free_header = FreePageHeader{
      .type = FREE_PAGE_TYPE,
      .next_free = header_.free_list_head,
  };
  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_written = ::pwrite(fd_.Get(), &free_header, sizeof(free_header), offset);
  if (bytes_written < 0) {
    return ErrnoStatus("pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != sizeof(free_header)) {
    return Status::IoError("short write on a free page header");
  }

  free_pages_.insert(page_id);
  header_.free_list_head = page_id;
  return WriteHeader();
}

auto DiskManager::GetRootPageId() const -> page_id_t { return header_.root_page_id; }

auto DiskManager::SetRootPageId(page_id_t root_page_id) -> Status {
  header_.root_page_id = root_page_id;
  return WriteHeader();
}

auto DiskManager::Sync() const -> Status {
  TINYDB_CHECK(fd_.Valid(), "syncing a closed disk manager");

  if (::fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  return {};
}

auto DiskManager::WriteHeader() const -> Status {
  TINYDB_CHECK(fd_.Valid(), "writing header to a closed disk manager");

  auto header_page = std::array<char, PAGE_SIZE>{};
  std::memcpy(header_page.data(), &header_, sizeof(header_));

  const auto bytes_written = ::pwrite(fd_.Get(), header_page.data(), header_page.size(), 0);
  if (bytes_written < 0) {
    return ErrnoStatus("pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != PAGE_SIZE) {
    return Status::IoError("short write on the header page");
  }
  return {};
}

auto DiskManager::ReadPage(page_id_t page_id, char *data) const -> Status {
  TINYDB_CHECK(fd_.Valid(), "reading from a closed disk manager");
  TINYDB_CHECK(!free_pages_.contains(page_id), "reading a freed page");

  if (page_id >= header_.next_page_id) {
    return Status::InvalidArgument("reading a page that was never allocated");
  }

  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_read = ::pread(fd_.Get(), data, PAGE_SIZE, offset);
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

  if (page_id >= header_.next_page_id) {
    return Status::InvalidArgument("writing a page that was never allocated");
  }

  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_written = ::pwrite(fd_.Get(), data, PAGE_SIZE, offset);
  if (bytes_written < 0) {
    return ErrnoStatus("pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != PAGE_SIZE) {
    return Status::IoError("short write on a page");
  }
  return {};
}

}  // namespace tinydb
