#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

namespace tinydb::io {

// Private POSIX syscall shim.
//
// Production calls go straight through to POSIX. Tests can install a hook to
// record durability ordering and inject syscall failures at exact points. This
// lives under src/ on purpose: it is not part of TinyDB's public API.
enum class Syscall {
  Open,
  Fstat,
  Pread,
  Pwrite,
  Fsync,
  Ftruncate,
  Flock,
};

struct Call {
  Syscall syscall;
  std::filesystem::path path;
  int fd{-1};
  int flags{0};
  mode_t mode{0};
  std::size_t size{0};
  std::uint64_t offset{0};
};

struct Fault {
  int error;
};

using TestHook = std::function<std::optional<Fault>(const Call &)>;

inline auto TestHookRef() -> TestHook & {
  static TestHook hook;
  return hook;
}

inline auto PathsByFd() -> std::unordered_map<int, std::filesystem::path> & {
  static auto paths = std::unordered_map<int, std::filesystem::path>{};
  return paths;
}

inline void SetTestHook(TestHook hook) { TestHookRef() = std::move(hook); }

inline void ClearTestHook() { TestHookRef() = {}; }

inline auto PathForFd(int fd) -> std::filesystem::path {
  const auto &paths = PathsByFd();
  const auto it = paths.find(fd);
  return it == paths.end() ? std::filesystem::path{} : it->second;
}

inline auto MaybeFault(const Call &call) -> bool {
  auto &hook = TestHookRef();
  if (!hook) {
    return false;
  }
  const auto fault = hook(call);
  if (!fault.has_value()) {
    return false;
  }
  errno = fault->error;
  return true;
}

inline auto Open(const std::filesystem::path &path, int flags, mode_t mode = 0) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Open, .path = path, .flags = flags, .mode = mode})) {
    return -1;
  }
  const int fd = (flags & O_CREAT) != 0 ? ::open(path.c_str(), flags, mode) : ::open(path.c_str(), flags);
  if (fd >= 0) {
    PathsByFd()[fd] = path;
  }
  return fd;
}

inline auto Fstat(int fd, struct stat *stat_buffer) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Fstat, .path = PathForFd(fd), .fd = fd})) {
    return -1;
  }
  return ::fstat(fd, stat_buffer);
}

inline auto Pread(int fd, void *data, std::size_t size, std::uint64_t offset) -> ssize_t {
  if (MaybeFault(Call{.syscall = Syscall::Pread, .path = PathForFd(fd), .fd = fd, .size = size, .offset = offset})) {
    return -1;
  }
  return ::pread(fd, data, size, static_cast<off_t>(offset));
}

inline auto Pwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> ssize_t {
  if (MaybeFault(Call{.syscall = Syscall::Pwrite, .path = PathForFd(fd), .fd = fd, .size = size, .offset = offset})) {
    return -1;
  }
  return ::pwrite(fd, data, size, static_cast<off_t>(offset));
}

inline auto Fsync(int fd) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Fsync, .path = PathForFd(fd), .fd = fd})) {
    return -1;
  }
  return ::fsync(fd);
}

inline auto Ftruncate(int fd, std::uint64_t size) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Ftruncate, .path = PathForFd(fd), .fd = fd, .size = size})) {
    return -1;
  }
  return ::ftruncate(fd, static_cast<off_t>(size));
}

inline auto Flock(int fd, int operation) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Flock, .path = PathForFd(fd), .fd = fd, .flags = operation})) {
    return -1;
  }
  return ::flock(fd, operation);
}

}  // namespace tinydb::io
