#include "io/file_io.h"

#include <tinydb/unique_fd.h>

#include "io/syscalls.h"

#include <fcntl.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace tinydb::io {

auto ErrnoStatus(std::string_view operation) -> Status {
  // Capture errno before constructing the message: library work may replace
  // the thread-local value and obscure the syscall that actually failed.
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

auto FullPread(int fd, void *data, std::size_t size, std::uint64_t offset) -> Result<std::size_t> {
  auto *bytes = static_cast<char *>(data);
  auto total = std::size_t{0};
  while (total < size) {
    const auto result = Pread(fd, bytes + total, size - total, offset + total);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(ErrnoStatus("pread"));
    }
    if (result == 0) {
      break;
    }
    total += static_cast<std::size_t>(result);
  }
  return total;
}

auto FullPwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> Status {
  const auto *bytes = static_cast<const char *>(data);
  auto written = std::size_t{0};
  while (written < size) {
    const auto result = Pwrite(fd, bytes + written, size - written, offset + written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ErrnoStatus("pwrite");
    }
    if (result == 0) {
      // A successful zero-byte result cannot advance a non-empty persistent
      // write.  Treat it as an environmental short write instead of spinning
      // forever at one file offset.
      return Status::IoError("pwrite made no progress");
    }
    written += static_cast<std::size_t>(result);
  }
  return {};
}

auto SyncParentDirectory(const std::filesystem::path &path) -> Status {
  auto parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  auto directory = UniqueFd(Open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory.Valid()) {
    return ErrnoStatus("open directory");
  }
  if (Fsync(directory.Get()) < 0) {
    return ErrnoStatus("fsync directory");
  }
  return {};
}

}  // namespace tinydb::io
