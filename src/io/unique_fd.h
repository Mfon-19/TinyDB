#pragma once

#include <utility>

namespace tinydb::io {

void Close(int fd) noexcept;

}  // namespace tinydb::io

namespace tinydb {

// Owns a POSIX file descriptor: closes it on destruction and on move
// assignment, so a descriptor cannot leak — not even when a constructor
// throws after acquiring one (members are destroyed; the half-built object's
// destructor is not run). -1 means "no descriptor".
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd &) = delete;
  auto operator=(const UniqueFd &) -> UniqueFd & = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  auto operator=(UniqueFd &&other) noexcept -> UniqueFd & {
    if (this != &other) {
      Close();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  ~UniqueFd() { Close(); }

  auto Get() const -> int { return fd_; }
  auto Valid() const -> bool { return fd_ >= 0; }

 private:
  void Close() noexcept {
    if (fd_ >= 0) {
      // A close error is unreportable here and durability never depends on
      // it: fsync is the durability point, and callers invoke it explicitly.
      const auto fd = std::exchange(fd_, -1);
      io::Close(fd);
    }
  }

  int fd_{-1};
};

}  // namespace tinydb
