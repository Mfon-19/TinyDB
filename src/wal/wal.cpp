#include <tinydb/wal.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tinydb {
namespace {

// "TDBW" — distinct from the database file's "TDB1" so the two can never be
// mistaken for one another. Doubles as the format version: a record-format
// change mints a new magic, and old logs are rejected below.
constexpr std::uint32_t WAL_MAGIC = 0x54444257U;

// The failing errno as an IoError status, tagged with the operation.
auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

}  // namespace

auto Wal::Open(const std::filesystem::path &wal_path) -> Result<Wal> {
  auto fd = UniqueFd(::open(wal_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));

  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }

  struct stat stat_buffer {};
  if (::fstat(fd.Get(), &stat_buffer) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }

  Wal wal(std::move(fd), static_cast<std::uint64_t>(stat_buffer.st_size));

  if (stat_buffer.st_size == 0) {
    // Fresh log: stamp the magic now and make it durable, so every later
    // open can tell a TinyDB log from an arbitrary file.
    const auto bytes_written = ::pwrite(wal.fd_.Get(), &WAL_MAGIC, sizeof(WAL_MAGIC), 0);
    if (bytes_written < 0) {
      return std::unexpected(ErrnoStatus("pwrite"));
    }
    if (static_cast<std::size_t>(bytes_written) != sizeof(WAL_MAGIC)) {
      return std::unexpected(Status::IoError("short write on the log header"));
    }
    if (::fsync(wal.fd_.Get()) < 0) {
      return std::unexpected(ErrnoStatus("fsync"));
    }
    wal.size_bytes_ = sizeof(WAL_MAGIC);
    return wal;
  }

  // Existing log: verify it is ours. Recover() already ran, so anything odd
  // here is a foreign or mangled file, not a crash artifact — reject it
  // rather than appending after bytes we cannot vouch for.
  std::uint32_t magic = 0;
  const auto bytes_read = ::pread(wal.fd_.Get(), &magic, sizeof(magic), 0);
  if (bytes_read < 0) {
    return std::unexpected(ErrnoStatus("pread"));
  }
  if (static_cast<std::size_t>(bytes_read) != sizeof(magic) || magic != WAL_MAGIC) {
    return std::unexpected(Status::InvalidArgument("not a TinyDB write-ahead log: " + wal_path.string()));
  }
  return wal;
}
}  // namespace tinydb
