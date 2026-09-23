#pragma once

#include "tinydb/detail/file.h"
#include "tinydb/storage/wal_codec.h"
#include <cstdint>
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

  [[nodiscard]] auto Salt() const noexcept -> std::uint32_t { return salt_; }

  auto Append(std::span<const char> record) -> Status;
  auto Sync() const -> Status;
  // Starts a new log over the old one. The next Sync makes it durable.
  auto Reset() -> Status;
  [[nodiscard]] auto Validate() const -> Result<PageMap>;

private:
  explicit Wal(detail::File file) noexcept : file_(std::move(file)) {}

  detail::File file_;
  off_t end_ = 0;
  std::uint32_t salt_ = 0;
};

} // namespace tinydb::storage
