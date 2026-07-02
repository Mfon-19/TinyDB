#include <tinydb/check.h>
#include <tinydb/disk_manager.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace tinydb {
static constexpr std::uint32_t FILE_MAGIC = 0x54444231U;

// Reads just the free-list header at the front of a free page.
static auto ReadFreePageHeader(int fd, page_id_t page_id) -> FreePageHeader {
  FreePageHeader header{};
  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_read = ::pread(fd, &header, sizeof(header), offset);
  if (bytes_read < 0) {
    throw std::system_error(errno, std::generic_category(), "pread");
  }
  if (static_cast<std::size_t>(bytes_read) != sizeof(header)) {
    throw std::runtime_error("short read on a free page header");
  }
  return header;
}

DiskManager::DiskManager(const std::filesystem::path &path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "open");
  }

  struct stat stat_buffer {};
  if (::fstat(fd_, &stat_buffer) < 0) {
    throw std::system_error(errno, std::generic_category(), "fstat");
  }

  if (stat_buffer.st_size == 0) {
    header_ = FileHeader{
        .magic = FILE_MAGIC,
        .page_size = PAGE_SIZE,
        .root_page_id = HEADER_PAGE_ID,
        .next_page_id = FIRST_DATA_PAGE_ID,
        .free_list_head = HEADER_PAGE_ID,
    };

    WriteHeader();
    return;
  }

  const auto bytes_read = ::pread(fd_, &header_, sizeof(header_), 0);
  if (bytes_read < 0) {
    throw std::system_error(errno, std::generic_category(), "pread");
  }

  // A file too short to hold a header cannot be a database either.
  if (static_cast<std::size_t>(bytes_read) != sizeof(header_) || header_.magic != FILE_MAGIC ||
      header_.page_size != PAGE_SIZE) {
    throw std::runtime_error("not a TinyDB database file: " + path.string());
  }

  // Rebuild the in-memory free-page set. Walking the list here also proves
  // it is acyclic and points only at allocated pages marked free.
  auto free_page_id = header_.free_list_head;
  while (free_page_id != HEADER_PAGE_ID) {
    if (free_page_id >= header_.next_page_id || free_pages_.contains(free_page_id)) {
      throw std::runtime_error("corrupt free list in " + path.string());
    }
    const auto free_header = ReadFreePageHeader(fd_, free_page_id);
    if (free_header.type != FREE_PAGE_TYPE) {
      throw std::runtime_error("corrupt free list in " + path.string());
    }
    free_pages_.insert(free_page_id);
    free_page_id = free_header.next_free;
  }
}

DiskManager::DiskManager(DiskManager &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), header_(other.header_), free_pages_(std::move(other.free_pages_)) {}

auto DiskManager::operator=(DiskManager &&other) noexcept -> DiskManager & {
  if (this != &other) {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }

    fd_ = std::exchange(other.fd_, -1);
    header_ = other.header_;
    free_pages_ = std::move(other.free_pages_);
  }

  return *this;
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
  }
}

auto DiskManager::AllocatePage() -> page_id_t {
  TINYDB_CHECK(fd_ >= 0, "allocating a page on a closed disk manager");

  // Pop the most recently freed page when one is available.
  if (header_.free_list_head != HEADER_PAGE_ID) {
    const auto page_id = header_.free_list_head;
    const auto free_header = ReadFreePageHeader(fd_, page_id);
    TINYDB_CHECK(free_header.type == FREE_PAGE_TYPE, "free list head is not a free page");

    header_.free_list_head = free_header.next_free;
    free_pages_.erase(page_id);
    WriteHeader();
    return page_id;
  }

  // Otherwise grow the file by one page.
  const auto page_id = header_.next_page_id;
  const auto new_size = static_cast<off_t>((page_id + 1) * PAGE_SIZE);

  if (::ftruncate(fd_, new_size) < 0) {
    throw std::system_error(errno, std::generic_category(), "ftruncate");
  }

  ++header_.next_page_id;
  WriteHeader();

  return page_id;
}

void DiskManager::FreePage(page_id_t page_id) {
  TINYDB_CHECK(fd_ >= 0, "freeing a page on a closed disk manager");
  const bool allocated = page_id != HEADER_PAGE_ID && page_id < header_.next_page_id;
  TINYDB_CHECK(allocated, "freeing a page that was never allocated");
  TINYDB_CHECK(!free_pages_.contains(page_id), "double free of a page");

  const auto free_header = FreePageHeader{
      .type = FREE_PAGE_TYPE,
      .next_free = header_.free_list_head,
  };
  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_written = ::pwrite(fd_, &free_header, sizeof(free_header), offset);
  if (bytes_written < 0) {
    throw std::system_error(errno, std::generic_category(), "pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != sizeof(free_header)) {
    throw std::runtime_error("short write on a free page header");
  }

  free_pages_.insert(page_id);
  header_.free_list_head = page_id;
  WriteHeader();
}

auto DiskManager::GetRootPageId() const -> page_id_t { return header_.root_page_id; }

void DiskManager::SetRootPageId(page_id_t root_page_id) {
  header_.root_page_id = root_page_id;
  WriteHeader();
}

void DiskManager::Sync() const {
  TINYDB_CHECK(fd_ >= 0, "syncing a closed disk manager");

  if (::fsync(fd_) < 0) {
    throw std::system_error(errno, std::generic_category(), "fsync");
  }
}

void DiskManager::WriteHeader() const {
  TINYDB_CHECK(fd_ >= 0, "writing header to a closed disk manager");

  auto header_page = std::array<char, PAGE_SIZE>{};
  std::memcpy(header_page.data(), &header_, sizeof(header_));

  const auto bytes_written = ::pwrite(fd_, header_page.data(), header_page.size(), 0);
  if (bytes_written < 0) {
    throw std::system_error(errno, std::generic_category(), "pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != PAGE_SIZE) {
    throw std::runtime_error("short write on the header page");
  }
}

auto DiskManager::ReadPage(page_id_t page_id, char *data) const -> void {
  TINYDB_CHECK(fd_ >= 0, "reading from a closed disk manager");
  TINYDB_CHECK(!free_pages_.contains(page_id), "reading a freed page");

  if (page_id >= header_.next_page_id) {
    throw std::out_of_range("page has not been allocated");
  }

  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_read = ::pread(fd_, data, PAGE_SIZE, offset);
  if (bytes_read < 0) {
    throw std::system_error(errno, std::generic_category(), "pread");
  }
  if (static_cast<std::size_t>(bytes_read) != PAGE_SIZE) {
    throw std::runtime_error("short read on a page");
  }
}

auto DiskManager::WritePage(page_id_t page_id, const char *data) const -> void {
  TINYDB_CHECK(fd_ >= 0, "writing to a closed disk manager");
  TINYDB_CHECK(!free_pages_.contains(page_id), "writing to a freed page");

  if (page_id >= header_.next_page_id) {
    throw std::out_of_range("page has not been allocated");
  }

  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  const auto bytes_written = ::pwrite(fd_, data, PAGE_SIZE, offset);
  if (bytes_written < 0) {
    throw std::system_error(errno, std::generic_category(), "pwrite");
  }
  if (static_cast<std::size_t>(bytes_written) != PAGE_SIZE) {
    throw std::runtime_error("short write on a page");
  }
}

}  // namespace tinydb
