#include "tinydb/detail/file.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace tinydb::detail {

auto SystemError(std::string_view operation, int error) -> Status {
  return Status::IoError(
      std::format("{}: {}", operation, std::strerror(error)));
}

auto File::Open(std::string_view path, int flags) -> Result<File> {
  const std::string name{path};
  const int fd = open(name.c_str(), flags | O_CLOEXEC, 0644);
  if (fd == -1) {
    return Err(SystemError("failed to open " + name, errno));
  }
  return File(fd);
}

File::~File() {
  if (fd_ != -1) {
    close(fd_);
  }
}

File::File(File &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

auto File::operator=(File &&other) noexcept -> File & {
  if (this != &other) {
    if (fd_ != -1) {
      close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

auto File::Size() const -> Result<off_t> {
  struct stat info {};
  if (fstat(fd_, &info) == -1) {
    return Err(SystemError("failed to inspect file", errno));
  }
  if (info.st_size < 0) {
    return Err(Status::Corruption("invalid file size"));
  }
  return info.st_size;
}

auto File::Read(off_t offset, std::span<char> bytes) const -> Status {
  while (!bytes.empty()) {
    const auto count = pread(fd_, bytes.data(), bytes.size(), offset);
    if (count > 0) {
      offset += count;
      bytes = bytes.subspan(static_cast<std::size_t>(count));
    } else if (count == -1 && errno == EINTR) {
      continue;
    } else if (count == 0) {
      return Status::IoError("unexpected end of file");
    } else {
      return SystemError("failed to read file", errno);
    }
  }
  return {};
}

auto File::Write(off_t offset, std::span<const char> bytes) const -> Status {
  while (!bytes.empty()) {
    const auto count = pwrite(fd_, bytes.data(), bytes.size(), offset);
    if (count > 0) {
      offset += count;
      bytes = bytes.subspan(static_cast<std::size_t>(count));
    } else if (count == -1 && errno == EINTR) {
      continue;
    } else {
      return SystemError("failed to write file", count == 0 ? EIO : errno);
    }
  }
  return {};
}

auto File::Sync() const -> Status {
  while (fsync(fd_) == -1) {
    if (errno != EINTR) {
      return SystemError("failed to sync file", errno);
    }
  }
  return {};
}

auto File::Truncate() const -> Status {
  while (ftruncate(fd_, 0) == -1) {
    if (errno != EINTR) {
      return SystemError("failed to truncate file", errno);
    }
  }
  return {};
}

}
