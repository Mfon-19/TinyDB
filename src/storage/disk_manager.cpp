#include "tinydb/storage/disk_manager.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace tinydb::storage {

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
    return Err(Status::IoError(
        std::format("an io error occured: {}", strerror(errno))));
  }

  return DiskManager{fd};
}

Status DiskManager::Write(const std::string_view buffer) {
  std::size_t written = 0;

  while (written < buffer.size()) {
    const ssize_t count =
        write(fd_, buffer.data() + written, buffer.size() - written);

    if (count > 0) {
      written += count;
    }

    if (count == -1 && errno == EINTR) {
      continue;
    }
  }

  if (written < buffer.size()) {
    return Status::IoError(
        std::format("failed to write whole buffer: {}", strerror(errno)));
  }

  return {};
}

Status DiskManager::Read(std::string &buffer) {
  const ssize_t count = pread(fd_, buffer.data(), buffer.size(), 0);
  if (count == -1) {
    return Status::IoError(
        std::format("failed to read from file: {}", strerror(errno)));
  }

  buffer.resize(count);
  return {};
}

} // namespace tinydb::storage