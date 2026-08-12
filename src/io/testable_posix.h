#pragma once

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <variant>

namespace tinydb::io {

// Private POSIX boundary. Production calls go directly to POSIX. Tests can
// observe calls and inject failures to verify crash safety and durability
// ordering.
enum class Syscall {
  Open,
  Fstat,
#if defined(__linux__) && defined(STATX_DIOALIGN)
  Statx,
#endif
  Pread,
  Pwrite,
  Pwritev,
  Fsync,
  Ftruncate,
  Flock,
};

struct Call {
  Syscall syscall;
  std::filesystem::path path;
  int flags{0};
  const void *data{nullptr};
  std::size_t size{0};
  std::uint64_t offset{0};
  const struct iovec *vectors{nullptr};
  std::size_t vector_count{0};

  // This view is valid only while the test hook runs.
  auto Vectors() const noexcept -> std::span<const struct iovec> { return {vectors, vector_count}; }
};

struct Fault {
  int error;
};

// Limit one real vectored write. Tests use this to exercise retry behavior
// after a short write without replacing the underlying file.
struct WriteLimit {
  std::size_t bytes;
};

using TestAction = std::variant<Fault, WriteLimit>;
using TestHook = std::function<std::optional<TestAction>(const Call &)>;

void SetTestHook(TestHook hook);
void ClearTestHook();
auto TestHookInstalledForTesting() noexcept -> bool;

auto Open(const std::filesystem::path &path, int flags, mode_t mode = 0) -> int;
auto Fstat(int fd, struct stat *stat_buffer) -> int;
#if defined(__linux__) && defined(STATX_DIOALIGN)
auto Statx(int fd, const char *path, int flags, unsigned int mask, struct statx *statx_buffer) -> int;
#endif
auto Pread(int fd, void *data, std::size_t size, std::uint64_t offset) -> ssize_t;
auto Pwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> ssize_t;
auto Pwritev(int fd, std::span<const struct iovec> vectors, std::uint64_t offset) -> ssize_t;
auto Fsync(int fd) -> int;
auto Ftruncate(int fd, std::uint64_t size) -> int;
auto Flock(int fd, int operation) -> int;

}  // namespace tinydb::io
