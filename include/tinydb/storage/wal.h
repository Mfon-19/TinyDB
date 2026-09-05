#pragma once

#include "tinydb/detail/file.h"
#include "tinydb/storage/wal_codec.h"
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace tinydb::storage {

class Wal {
public:
  [[nodiscard]] static auto Open(std::string_view database_path) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;
  Wal(Wal &&other) noexcept = default;
  auto operator=(Wal &&other) noexcept -> Wal & = default;

  [[nodiscard]] auto Empty() const noexcept -> bool { return end_ == 0; }

  auto Append(std::span<const char> record) -> Status;
  auto Sync() const -> Status;
  auto Reset() -> Status;
  [[nodiscard]] auto Validate() const -> Result<PageMap>;

private:
  Wal(detail::File file, off_t size) noexcept
      : file_(std::move(file)), end_(size) {}

  detail::File file_;
  off_t end_ = 0;
};

} // namespace tinydb::storage
