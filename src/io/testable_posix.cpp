#include "io/testable_posix.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <memory>
#include <mutex>
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

auto MaybeFault(Call call, int fd = -1) -> bool {
  auto &state = TestState();
  if (!state.hook_active.load(std::memory_order_acquire)) {
    return false;
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
    return false;
  }
  const auto fault = (*hook)(call);
  if (!fault) {
    return false;
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

auto Open(const std::filesystem::path &path, int flags, mode_t mode) -> int {
  if (MaybeFault(Call{.syscall = Syscall::Open, .path = path})) {
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

auto Pread(int fd, void *data, std::size_t size, std::uint64_t offset) -> ssize_t {
  if (MaybeFault(Call{.syscall = Syscall::Pread, .path = {}, .offset = offset}, fd)) {
    return -1;
  }
  return ::pread(fd, data, size, static_cast<off_t>(offset));
}

auto Pwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> ssize_t {
  if (MaybeFault(Call{.syscall = Syscall::Pwrite, .path = {}, .offset = offset}, fd)) {
    return -1;
  }
  return ::pwrite(fd, data, size, static_cast<off_t>(offset));
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
