#include "tinydb/storage/wal.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace tinydb::storage {

namespace {

auto SystemError(std::string_view operation, int error) -> Status {
  return Status::IoError(
      std::format("{}: {}", operation, std::strerror(error)));
}

auto FileSize(int fd) -> Result<off_t> {
  struct stat info {};
  if (fstat(fd, &info) == -1) {
    return Err(SystemError("failed to inspect WAL", errno));
  }
  if (info.st_size < 0) {
    return Err(Status::Corruption("invalid WAL file size"));
  }
  return info.st_size;
}

auto IoSize(std::size_t remaining) -> std::size_t {
  return std::min(
      remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
}

} // namespace

auto Wal::Open(std::string_view database_path) -> Result<Wal> {
  const std::string path = std::string{database_path} + "-wal";
  const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd == -1) {
    return Err(SystemError("failed to open WAL", errno));
  }
  Wal wal(fd);
  auto size = FileSize(fd);
  if (!size) {
    return Err(std::move(size.error()));
  }
  wal.end_ = *size;
  return wal;
}

Wal::~Wal() {
  if (fd_ != -1) {
    close(fd_);
  }
}

Wal::Wal(Wal &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), end_(std::exchange(other.end_, 0)) {}

Wal &Wal::operator=(Wal &&other) noexcept {
  if (this != &other) {
    if (fd_ != -1) {
      close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
    end_ = std::exchange(other.end_, 0);
  }
  return *this;
}

Status Wal::Append(std::span<const char> record) {
  if (record.empty()) {
    return Status::InvalidArgument("cannot append an empty WAL record");
  }
  if (record.size() >
      static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max() - end_)) {
    return Status::ResourceExhausted("WAL file is too large");
  }
  std::size_t written = 0;
  while (written < record.size()) {
    const ssize_t count =
        pwrite(fd_, record.data() + written, IoSize(record.size() - written),
               end_ + static_cast<off_t>(written));
    if (count > 0) {
      written += static_cast<std::size_t>(count);
    } else if (count == -1 && errno == EINTR) {
      continue;
    } else if (count == 0) {
      return Status::IoError(EIO);
    } else {
      return Status::IoError(errno);
    }
  }
  end_ += static_cast<off_t>(record.size());
  return {};
}

Status Wal::Sync() const {
  while (fsync(fd_) == -1) {
    if (errno != EINTR) {
      return Status::IoError(errno);
    }
  }
  return {};
}

Status Wal::Reset() {
  while (ftruncate(fd_, 0) == -1) {
    if (errno != EINTR) {
      return Status::IoError(errno);
    }
  }
  if (auto status = Sync(); !status.Ok()) {
    return status;
  }
  end_ = 0;
  return {};
}

auto Wal::Validate() const -> Result<WalPages> {
  auto size = FileSize(fd_);
  if (!size) {
    return Err(std::move(size.error()));
  }
  std::vector<char> bytes;
  if (static_cast<std::uintmax_t>(*size) > bytes.max_size()) {
    return Err(Status::ResourceExhausted("WAL file is too large to read"));
  }
  bytes.resize(static_cast<std::size_t>(*size));
  std::size_t read_bytes = 0;
  while (read_bytes < bytes.size()) {
    const ssize_t count =
        pread(fd_, bytes.data() + read_bytes, IoSize(bytes.size() - read_bytes),
              static_cast<off_t>(read_bytes));
    if (count > 0) {
      read_bytes += static_cast<std::size_t>(count);
    } else if (count == -1 && errno == EINTR) {
      continue;
    } else if (count == 0) {
      return Err(Status::IoError("unexpected end of file while reading WAL"));
    } else {
      return Err(SystemError("failed to read WAL", errno));
    }
  }
  return DecodeWal(bytes);
}

} // namespace tinydb::storage
