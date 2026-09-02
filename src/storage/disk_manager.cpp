#include "tinydb/storage/disk_manager.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace tinydb::storage {

namespace {

auto PageOffset(PageId page_id) noexcept -> off_t {
  return static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
}

auto SystemError(std::string_view operation, int error) -> Status {
  return Status::IoError(
      std::format("{}: {}", operation, std::strerror(error)));
}

} // namespace

DiskManager::~DiskManager() {
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
}

DiskManager::DiskManager(DiskManager &&other) noexcept
    : fd_(other.fd_), next_page_id_(other.next_page_id_) {
  other.fd_ = -1;
}

DiskManager &DiskManager::operator=(DiskManager &&other) noexcept {
  if (this != &other) {
    if (fd_ != -1) {
      close(fd_);
    }
    fd_ = other.fd_;
    next_page_id_ = other.next_page_id_;
    other.fd_ = -1;
  }
  return *this;
}

Result<DiskManager> DiskManager::Open(const std::string_view name) {
  const std::string path{name};

  const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd == -1) {
    return Err(SystemError("failed to open database", errno));
  }

  struct stat file_status {};
  if (fstat(fd, &file_status) == -1) {
    const int error = errno;
    close(fd);
    return Err(SystemError("failed to inspect database", error));
  }

  const auto file_size = static_cast<std::uint64_t>(file_status.st_size);
  const auto page_count =
      (file_size + PAGE_SIZE - 1) / static_cast<std::uint64_t>(PAGE_SIZE);
  if (page_count >= INVALID_PAGE_ID) {
    close(fd);
    return Err(Status::ResourceExhausted("database has too many pages"));
  }

  const auto next_page_id =
      static_cast<PageId>(page_count == 0 ? 1 : page_count);
  return DiskManager{fd, next_page_id};
}

Result<PageId> DiskManager::AllocatePage() {
  if (next_page_id_ == INVALID_PAGE_ID) {
    return Err(Status::ResourceExhausted("database has too many pages"));
  }
  return next_page_id_++;
}

Status DiskManager::WritePage(PageId page_id, const PageBytes &page) {
  const off_t offset = PageOffset(page_id);

  std::size_t written = 0;
  while (written < page.size()) {
    const auto current_offset = offset + static_cast<off_t>(written);
    const ssize_t count = pwrite(fd_, page.data() + written,
                                 page.size() - written, current_offset);

    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }

    if (count == -1 && errno == EINTR) {
      continue;
    }

    if (count == 0) {
      return Status::IoError("page write made no progress");
    }

    return SystemError("failed to write page", errno);
  }

  if (page_id >= next_page_id_ && page_id != INVALID_PAGE_ID) {
    next_page_id_ = page_id + 1;
  }

  return {};
}

Status DiskManager::ReadPage(PageId page_id, PageBytes &page) const {
  const off_t offset = PageOffset(page_id);

  std::size_t read_bytes = 0;
  while (read_bytes < page.size()) {
    const auto current_offset = offset + static_cast<off_t>(read_bytes);
    const ssize_t count = pread(fd_, page.data() + read_bytes,
                                page.size() - read_bytes, current_offset);

    if (count > 0) {
      read_bytes += static_cast<std::size_t>(count);
      continue;
    }

    if (count == -1 && errno == EINTR) {
      continue;
    }

    if (count == 0) {
      return Status::IoError(
          std::format("unexpected end of file while reading page {}", page_id));
    }

    return SystemError("failed to read page", errno);
  }

  return {};
}

} // namespace tinydb::storage
