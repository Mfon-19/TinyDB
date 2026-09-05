#pragma once

#include "tinydb/status.h"
#include <span>
#include <string_view>
#include <sys/types.h>

namespace tinydb::detail {

class File {
public:
  [[nodiscard]] static auto Open(std::string_view path,
                                 int flags) -> Result<File>;
  ~File();
  File(const File &) = delete;
  auto operator=(const File &) -> File & = delete;
  File(File &&other) noexcept;
  auto operator=(File &&other) noexcept -> File &;

  [[nodiscard]] auto Get() const noexcept -> int { return fd_; }
  [[nodiscard]] auto Size() const -> Result<off_t>;
  auto Read(off_t offset, std::span<char> bytes) const -> Status;
  auto Write(off_t offset, std::span<const char> bytes) const -> Status;
  auto Sync() const -> Status;
  auto Truncate() const -> Status;

private:
  explicit File(int fd) noexcept : fd_(fd) {}
  int fd_;
};

auto SystemError(std::string_view operation, int error) -> Status;

}
