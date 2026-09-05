#include "tinydb/storage/disk_manager.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <sys/file.h>
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

Status SyncFile(int fd) {
  while (fsync(fd) == -1) {
    if (errno != EINTR) {
      return Status::IoError(errno);
    }
  }
  return {};
}

} // namespace

DiskManager::~DiskManager() {
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
}

DiskManager::DiskManager(DiskManager &&other) noexcept
    : fd_(other.fd_), page_count_(other.page_count_) {
  other.fd_ = -1;
}

DiskManager &DiskManager::operator=(DiskManager &&other) noexcept {
  if (this != &other) {
    if (fd_ != -1) {
      close(fd_);
    }
    fd_ = other.fd_;
    page_count_ = other.page_count_;
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

  if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
    const int error = errno;
    close(fd);
    if (error == EWOULDBLOCK) {
      return Err(Status::ResourceExhausted("database is already open"));
    }
    return Err(SystemError("failed to lock database", error));
  }

  struct stat file_status {};
  if (fstat(fd, &file_status) == -1) {
    const int error = errno;
    close(fd);
    return Err(SystemError("failed to inspect database", error));
  }
  if (file_status.st_nlink != 1) {
    close(fd);
    return Err(
        Status::InvalidArgument("database must have exactly one hard link"));
  }

  const auto file_size = static_cast<std::uint64_t>(file_status.st_size);
  const auto page_count =
      (file_size + PAGE_SIZE - 1) / static_cast<std::uint64_t>(PAGE_SIZE);
  if (page_count >= INVALID_PAGE_ID) {
    close(fd);
    return Err(Status::ResourceExhausted("database has too many pages"));
  }

  return DiskManager{fd, static_cast<PageId>(page_count)};
}

Status DiskManager::Sync() const {
  return SyncFile(fd_);
}

Status SyncParentDirectory(std::string_view path) {
  const auto slash = path.find_last_of('/');
  const std::string parent = slash == std::string_view::npos
                                 ? "."
                                 : std::string{path.substr(0, slash == 0 ? 1 : slash)};
  const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd == -1) {
    return SystemError("failed to open database directory", errno);
  }
  auto status = SyncFile(fd);
  close(fd);
  return status;
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
      return Status::IoError(EIO);
    }

    return Status::IoError(errno);
  }

  if (page_id >= page_count_ && page_id != INVALID_PAGE_ID) {
    page_count_ = page_id + 1;
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
