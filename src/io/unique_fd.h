#pragma once

#include <utility>

namespace tinydb::io {

void Close(int fd) noexcept;

}  // namespace tinydb::io
namespace tinydb {

/*
** Own one POSIX descriptor. Move assignment and destruction close the old
** descriptor, including when construction of an enclosing object fails after
** this member has acquired it. A value of -1 means that no descriptor is held.
*/
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
