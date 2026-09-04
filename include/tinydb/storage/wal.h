#pragma once

#include "tinydb/storage/wal_codec.h"
#include <string_view>
#include <sys/types.h>

namespace tinydb::storage {

class Wal {
public:
  static auto Open(std::string_view database_path) -> Result<Wal>;
  ~Wal();

  Wal(const Wal &) = delete;
  Wal &operator=(const Wal &) = delete;
  Wal(Wal &&other) noexcept;
  Wal &operator=(Wal &&other) noexcept;

  [[nodiscard]] bool Empty() const noexcept { return end_ == 0; }

  Status Append(std::span<const char> record);
  Status Sync() const;
  Status Reset();
  auto Validate() const -> Result<WalPages>;

private:
  explicit Wal(int fd) noexcept : fd_(fd) {}

  int fd_;
  off_t end_ = 0;
};

} // namespace tinydb::storage
