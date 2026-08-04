#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace tinydb::io {

// Private POSIX boundary. Production calls go directly to POSIX. Tests can
// observe calls and inject failures to verify crash safety and durability
// ordering.
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
  std::uint64_t offset{0};
};

struct Fault {
  int error;
};

using TestHook = std::function<std::optional<Fault>(const Call &)>;

void SetTestHook(TestHook hook);
void ClearTestHook();

auto Open(const std::filesystem::path &path, int flags, mode_t mode = 0) -> int;
auto Fstat(int fd, struct stat *stat_buffer) -> int;
auto Pread(int fd, void *data, std::size_t size, std::uint64_t offset) -> ssize_t;
auto Pwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> ssize_t;
auto Fsync(int fd) -> int;
auto Ftruncate(int fd, std::uint64_t size) -> int;
auto Flock(int fd, int operation) -> int;

}  // namespace tinydb::io
