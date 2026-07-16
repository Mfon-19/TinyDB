#pragma once

#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace tinydb {

class Wal {
 public:
  static auto PathFor(const std::filesystem::path &db_path) -> std::filesystem::path;
  static auto Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status;
  static auto Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid) -> Result<Wal>;

  Wal(const Wal &) = delete;
  auto operator=(const Wal &) -> Wal & = delete;

  Wal(Wal &&) noexcept = default;
  auto operator=(Wal &&) noexcept -> Wal & = default;
  ~Wal() = default;

  void AppendPageImage(page_id_t page_id, const char *data);
  void DiscardPending();
  auto Commit() -> Status;
  auto SizeBytes() const -> std::uint64_t;
  auto Reset() -> Status;

 private:
  Wal(UniqueFd fd, std::uint64_t size_bytes) : fd_(std::move(fd)), size_bytes_(size_bytes) {}

  UniqueFd fd_;
  std::uint64_t size_bytes_{0};
  std::uint64_t next_transaction_id_{1};
  std::size_t pending_image_count_{0};
  std::vector<char> pending_;
};
}  // namespace tinydb
