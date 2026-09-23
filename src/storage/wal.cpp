#include "tinydb/storage/wal.h"
#include <cstdint>
#include <fcntl.h>
#include <random>
#include <string>
#include <utility>

namespace tinydb::storage {

auto Wal::Open(std::string_view database_path) -> Result<Wal> {
  auto file =
      detail::File::Open(std::string{database_path} + "-wal", O_CREAT | O_RDWR);
  if (!file) {
    return Err(std::move(file.error()));
  }
  return Wal(std::move(*file));
}

auto Wal::Append(std::span<const char> record) -> Status {
  if (record.empty()) {
    return Status::InvalidArgument("cannot append an empty WAL record");
  }
  if (auto status = file_.Write(end_, record); !status.Ok()) {
    return status;
  }
  end_ += static_cast<off_t>(record.size());
  return {};
}

auto Wal::Sync() const -> Status { return file_.Sync(/*data_only=*/true); }

auto Wal::Reset() -> Status {
  const auto salt = static_cast<std::uint32_t>(std::random_device{}());
  if (auto status = file_.Write(0, EncodeWalHeader(salt)); !status.Ok()) {
    return status;
  }
  end_ = WAL_HEADER_SIZE;
  salt_ = salt;
  return {};
}

auto Wal::Validate() const -> Result<PageMap> {
  auto size = file_.Size();
  if (!size) {
    return Err(std::move(size.error()));
  }
  std::vector<char> bytes;
  if (static_cast<std::uintmax_t>(*size) > bytes.max_size()) {
    return Err(Status::ResourceExhausted("WAL file is too large to read"));
  }
  bytes.resize(static_cast<std::size_t>(*size));
  if (auto status = file_.Read(0, bytes); !status.Ok()) {
    return Err(std::move(status));
  }
  return DecodeWal(bytes);
}

} // namespace tinydb::storage
