#include "io/testable_posix.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>

namespace tinydb::io {
namespace {

struct State {
  std::mutex mutex;
  std::shared_ptr<const TestHook> hook;
  std::atomic_bool hook_active{false};
  std::unordered_map<int, std::filesystem::path> paths_by_fd;
};

auto TestState() -> State & {
  static State state;
  return state;
}

auto RequestedAction(Call call, int fd = -1) -> std::optional<TestAction> {
  auto &state = TestState();
  if (!state.hook_active.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  auto hook = std::shared_ptr<const TestHook>{};
  {
    auto lock = std::lock_guard(state.mutex);
    if (fd >= 0) {
      const auto path = state.paths_by_fd.find(fd);
      if (path != state.paths_by_fd.end()) {
        call.path = path->second;
      }
    }
    hook = state.hook;
  }
  if (!hook) {
    return std::nullopt;
  }
  return (*hook)(call);
}

auto MaybeFault(Call call, int fd = -1) -> bool {
  const auto action = RequestedAction(std::move(call), fd);
  if (!action) {
    return false;
  }
  const auto *const fault = std::get_if<Fault>(&*action);
  if (fault == nullptr) {
    errno = EINVAL;
    return true;
  }
  errno = fault->error;
  return true;
}

void Remember(int fd, const std::filesystem::path &path) {
  auto &state = TestState();
  auto lock = std::lock_guard(state.mutex);
  state.paths_by_fd.insert_or_assign(fd, path);
}

void Forget(int fd) {
  auto &state = TestState();
  auto lock = std::lock_guard(state.mutex);
  state.paths_by_fd.erase(fd);
}

}  // namespace

void SetTestHook(TestHook hook) {
  auto replacement = hook ? std::make_shared<const TestHook>(std::move(hook)) : nullptr;
  auto &state = TestState();
  auto previous = std::shared_ptr<const TestHook>{};
  {
    auto lock = std::lock_guard(state.mutex);
    previous = std::exchange(state.hook, std::move(replacement));
    state.hook_active.store(static_cast<bool>(state.hook), std::memory_order_release);
  }
}

void ClearTestHook() { SetTestHook({}); }

auto TestHookInstalledForTesting() noexcept -> bool { return TestState().hook_active.load(std::memory_order_acquire); }

auto Open(const std::filesystem::path &path, int flags, mode_t mode) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Open, .path = path, .flags = flags})) {
    return -1;
  }
  const auto fd = (flags & O_CREAT) != 0 ? ::open(path.c_str(), flags, mode) : ::open(path.c_str(), flags);
  if (fd >= 0) {
    Remember(fd, path);
  }
  return fd;
}

void Close(int fd) noexcept {
  Forget(fd);
  static_cast<void>(::close(fd));
}

auto Fstat(int fd, struct stat *stat_buffer) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Fstat, .path = {}}, fd)) {
    return -1;
  }
  return ::fstat(fd, stat_buffer);
}

#if defined(__linux__) && defined(STATX_DIOALIGN)
auto Statx(int fd, const char *path, int flags, unsigned int mask, struct statx *statx_buffer) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Statx, .path = {}}, fd)) {
    return -1;
  }
  return ::statx(fd, path, flags, mask, statx_buffer);
}
#endif

auto Pread(int fd, void *data, std::size_t size, std::uint64_t offset) -> ssize_t {
  if (MaybeFault(Call{.syscall = Syscall::Pread, .path = {}, .data = data, .size = size, .offset = offset}, fd)) {
    return -1;
  }
  return ::pread(fd, data, size, static_cast<off_t>(offset));
}

auto Pwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> ssize_t {
  if (MaybeFault(Call{.syscall = Syscall::Pwrite, .path = {}, .data = data, .size = size, .offset = offset}, fd)) {
    return -1;
  }
  return ::pwrite(fd, data, size, static_cast<off_t>(offset));
}

auto Pwritev(int fd, std::span<const struct iovec> vectors, std::uint64_t offset) -> ssize_t {
  if (vectors.empty() || vectors.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    errno = EINVAL;
    return -1;
  }
  auto size = std::size_t{0};
  for (const auto &vector : vectors) {
    if (vector.iov_len > std::numeric_limits<std::size_t>::max() - size) {
      errno = EINVAL;
      return -1;
    }
    size += vector.iov_len;
  }

  const auto action = RequestedAction(Call{.syscall = Syscall::Pwritev,
                                           .path = {},
                                           .data = vectors.front().iov_base,
                                           .size = size,
                                           .offset = offset,
                                           .vectors = vectors.data(),
                                           .vector_count = vectors.size()},
                                      fd);
  if (!action) {
    return ::pwritev(fd, vectors.data(), static_cast<int>(vectors.size()), static_cast<off_t>(offset));
  }
  if (const auto *const fault = std::get_if<Fault>(&*action); fault != nullptr) {
    errno = fault->error;
    return -1;
  }

  constexpr auto maximum_test_vectors = std::size_t{64};
  const auto limit = std::get<WriteLimit>(*action).bytes;
  if (limit == 0) {
    return 0;
  }
  if (vectors.size() > maximum_test_vectors) {
    errno = EINVAL;
    return -1;
  }
  auto clipped = std::array<struct iovec, maximum_test_vectors>{};
  auto remaining = std::min(limit, size);
  auto count = std::size_t{0};
  for (const auto &vector : vectors) {
    if (remaining == 0) {
      break;
    }
    const auto bytes = std::min(remaining, vector.iov_len);
    clipped[count++] = iovec{.iov_base = vector.iov_base, .iov_len = bytes};
    remaining -= bytes;
  }
  return ::pwritev(fd, clipped.data(), static_cast<int>(count), static_cast<off_t>(offset));
}

auto Fsync(int fd) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Fsync, .path = {}}, fd)) {
    return -1;
  }
  return ::fsync(fd);
}

auto Ftruncate(int fd, std::uint64_t size) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Ftruncate, .path = {}}, fd)) {
    return -1;
  }
  return ::ftruncate(fd, static_cast<off_t>(size));
}

auto Flock(int fd, int operation) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Flock, .path = {}}, fd)) {
    return -1;
  }
  return ::flock(fd, operation);
}

}  // namespace tinydb::io
