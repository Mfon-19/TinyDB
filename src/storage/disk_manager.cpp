#include <tinydb/disk_manager.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace tinydb {
static constexpr std::uint32_t FILE_MAGIC = 0x54444231U;

DiskManager::DiskManager(const std::filesystem::path &path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);

  struct stat stat_buffer {};
  ::fstat(fd_, &stat_buffer);

  if (stat_buffer.st_size == 0) {
    header_ = FileHeader{
        .magic = FILE_MAGIC,
        .page_size = PAGE_SIZE,
        .root_page_id = HEADER_PAGE_ID,
        .next_page_id = FIRST_DATA_PAGE_ID,
    };

    auto header_page = std::array<char, PAGE_SIZE>{};
    std::memcpy(header_page.data(), &header_, sizeof(header_));

    ::pwrite(fd_, header_page.data(), header_page.size(), 0);
    return;
  }

  ::pread(fd_, &header_, sizeof(header_), 0);
}

DiskManager::DiskManager(DiskManager &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), header_(other.header_) {}

auto DiskManager::operator=(DiskManager &&other) noexcept -> DiskManager & {
  if (this != &other) {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }

    fd_ = std::exchange(other.fd_, -1);
    header_ = other.header_;
  }

  return *this;
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
  }
}

// Increment next_page_id, then write to the database file
auto DiskManager::AllocatePage() -> page_id_t {
  const auto page_id = header_.next_page_id;
  ++header_.next_page_id;
  ::pwrite(fd_, &header_, sizeof(header_), 0);

  return page_id;
}

auto DiskManager::ReadPage(page_id_t page_id, char *data) const -> void {
  if (page_id >= header_.next_page_id) {
    throw std::out_of_range("page has not been allocated");
  }

  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  ::pread(fd_, data, PAGE_SIZE, offset);
}

auto DiskManager::WritePage(page_id_t page_id, const char *data) const -> void {
  if (page_id >= header_.next_page_id) {
    throw std::out_of_range("page has not been allocated");
  }

  const auto offset = static_cast<off_t>(page_id * PAGE_SIZE);
  ::pwrite(fd_, data, PAGE_SIZE, offset);
}

}  // namespace tinydb
