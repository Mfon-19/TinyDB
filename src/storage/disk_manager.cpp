#include "tinydb/storage/disk_manager.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace tinydb::storage {

namespace {

auto PageOffset(PageId page_id) -> Result<off_t> {
  constexpr auto max_offset =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  constexpr auto last_byte = static_cast<std::uint64_t>(PAGE_SIZE - 1);

  if (page_id > (max_offset - last_byte) / PAGE_SIZE) {
    return Err(Status::IoError("page offset is too large"));
  }

  return static_cast<off_t>(page_id * PAGE_SIZE);
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

DiskManager::DiskManager(DiskManager &&other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

DiskManager &DiskManager::operator=(DiskManager &&other) noexcept {
  if (this != &other) {
    if (fd_ != -1) {
      close(fd_);
    }
    fd_ = other.fd_;
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

  return DiskManager{fd};
}

Status DiskManager::WritePage(PageId page_id, const PageBytes &page) {
  auto offset = PageOffset(page_id);
  if (!offset) {
    return std::move(offset.error());
  }

  std::size_t written = 0;
  while (written < page.size()) {
    const auto current_offset = *offset + static_cast<off_t>(written);
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

  return {};
}

Status DiskManager::ReadPage(PageId page_id, PageBytes &page) const {
  auto offset = PageOffset(page_id);
  if (!offset) {
    return std::move(offset.error());
  }

  std::size_t read_bytes = 0;
  while (read_bytes < page.size()) {
    const auto current_offset = *offset + static_cast<off_t>(read_bytes);
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
