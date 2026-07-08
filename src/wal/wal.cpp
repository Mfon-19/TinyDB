#include <tinydb/check.h>
#include <tinydb/wal.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

// "TDBW" — distinct from the database file's "TDB1" so the two can never be
// mistaken for one another. Doubles as the format version: a record-format
// change mints a new magic, and old logs are rejected below.
constexpr std::uint32_t WAL_MAGIC = 0x54444257U;

// Record payloads. A page image is {type, page_id, 4096-byte post-image};
// a commit is just its type byte.
constexpr char PAGE_IMAGE_TYPE = 1;
constexpr char COMMIT_TYPE = 2;
constexpr std::size_t PAGE_IMAGE_DATA_OFFSET = sizeof(PAGE_IMAGE_TYPE) + sizeof(page_id_t);
constexpr std::uint32_t PAGE_IMAGE_PAYLOAD_SIZE = PAGE_IMAGE_DATA_OFFSET + PAGE_SIZE;
constexpr std::uint32_t COMMIT_PAYLOAD_SIZE = sizeof(COMMIT_TYPE);
constexpr std::size_t RECORD_FRAME_SIZE = 2 * sizeof(std::uint32_t);  // length + crc

// The failing errno as an IoError status, tagged with the operation.
auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

// CRC-32 (the reflected IEEE polynomial — the same function as zlib's
// crc32), table-driven, with the table built at compile time.
constexpr auto MakeCrc32Table() -> std::array<std::uint32_t, 256> {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t byte = 0; byte < table.size(); ++byte) {
    std::uint32_t remainder = byte;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 1U) != 0 ? 0xEDB88320U ^ (remainder >> 1U) : remainder >> 1U;
    }
    table[byte] = remainder;
  }
  return table;
}
constexpr auto CRC32_TABLE = MakeCrc32Table();

auto Crc32(const char *data, std::size_t size) -> std::uint32_t {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    const auto byte = static_cast<unsigned char>(data[i]);
    crc = CRC32_TABLE[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

// Appends one crc-framed record — {u32 payload_length | u32 crc32 | payload}
// — to an operation's pending buffer.
void AppendRecord(std::vector<char> &out, const char *payload, std::uint32_t payload_length) {
  const std::uint32_t crc = Crc32(payload, payload_length);
  std::array<char, RECORD_FRAME_SIZE> frame{};
  std::memcpy(frame.data(), &payload_length, sizeof(payload_length));
  std::memcpy(frame.data() + sizeof(payload_length), &crc, sizeof(crc));
  out.insert(out.end(), frame.begin(), frame.end());
  out.insert(out.end(), payload, payload + payload_length);
}

// pwrite the whole buffer, resuming after short writes and EINTR: POSIX lets
// a write stop early, and the log needs every byte.
auto FullPwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> Status {
  const auto *bytes = static_cast<const char *>(data);
  std::size_t written = 0;
  while (written < size) {
    const auto result = ::pwrite(fd, bytes + written, size - written, static_cast<off_t>(offset + written));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ErrnoStatus("pwrite");
    }
    written += static_cast<std::size_t>(result);
  }
  return {};
}

// pread up to size bytes, resuming after EINTR. Returns the bytes read:
// short only at end of file, which the recovery scan treats as a torn tail,
// not an error.
auto FullPread(int fd, void *data, std::size_t size, std::uint64_t offset) -> Result<std::size_t> {
  auto *bytes = static_cast<char *>(data);
  std::size_t total = 0;
  while (total < size) {
    const auto result = ::pread(fd, bytes + total, size - total, static_cast<off_t>(offset + total));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(ErrnoStatus("pread"));
    }
    if (result == 0) {
      break;
    }
    total += static_cast<std::size_t>(result);
  }
  return total;
}

}  // namespace

auto Wal::PathFor(const std::filesystem::path &db_path) -> std::filesystem::path {
  auto wal_path = db_path;
  wal_path += "-wal";  // concatenation ("data.db-wal"), not a new component
  return wal_path;
}

auto Wal::Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status {
  auto wal_fd = UniqueFd(::open(wal_path.c_str(), O_RDWR | O_CLOEXEC));
  if (!wal_fd.Valid()) {
    if (errno == ENOENT) {
      return {};  // no log, nothing to recover
    }
    return ErrnoStatus("open");
  }

  struct stat stat_buffer {};
  if (::fstat(wal_fd.Get(), &stat_buffer) < 0) {
    return ErrnoStatus("fstat");
  }
  const auto wal_size = static_cast<std::uint64_t>(stat_buffer.st_size);

  // A crash can beat the header to disk: Open() creates the file and only
  // then writes and fsyncs the magic. Clear the remnant so the next Open()
  // stamps it afresh.
  if (wal_size < sizeof(WAL_MAGIC)) {
    if (wal_size > 0 && ::ftruncate(wal_fd.Get(), 0) < 0) {
      return ErrnoStatus("ftruncate");
    }
    return {};
  }

  std::uint32_t magic = 0;
  const auto magic_read = FullPread(wal_fd.Get(), &magic, sizeof(magic), 0);
  if (!magic_read) {
    return magic_read.error();
  }
  if (*magic_read != sizeof(magic) || magic != WAL_MAGIC) {
    return Status::InvalidArgument("not a TinyDB write-ahead log: " + wal_path.string());
  }

  // Scan the records in order, buffering page images and applying each run
  // to the database when its commit record proves the run is complete. The
  // scan stops at the first sign of a torn tail: a truncated frame, a length
  // that cannot be real, or a CRC mismatch.
  UniqueFd db_fd;  // opened at the first commit, if any
  std::vector<std::pair<page_id_t, std::vector<char>>> run;
  bool applied = false;
  auto offset = static_cast<std::uint64_t>(sizeof(WAL_MAGIC));

  while (offset < wal_size) {
    std::array<std::uint32_t, 2> frame{};  // {payload_length, crc32}
    const auto frame_read = FullPread(wal_fd.Get(), frame.data(), sizeof(frame), offset);
    if (!frame_read) {
      return frame_read.error();
    }
    const auto payload_length = frame[0];
    const auto expected_crc = frame[1];
    if (*frame_read != sizeof(frame) || payload_length == 0 || payload_length > PAGE_IMAGE_PAYLOAD_SIZE ||
        offset + sizeof(frame) + payload_length > wal_size) {
      break;
    }

    std::vector<char> payload(payload_length);
    const auto payload_read = FullPread(wal_fd.Get(), payload.data(), payload_length, offset + sizeof(frame));
    if (!payload_read) {
      return payload_read.error();
    }
    if (*payload_read != payload_length || Crc32(payload.data(), payload_length) != expected_crc) {
      break;
    }
    offset += sizeof(frame) + payload_length;

    // A record whose CRC passes but whose contents make no sense is not a
    // torn write (a torn write fails the CRC): the log is corrupt, and
    // guessing which records to trust would be worse than refusing.
    switch (payload.front()) {
      case PAGE_IMAGE_TYPE: {
        if (payload_length != PAGE_IMAGE_PAYLOAD_SIZE) {
          return Status::Corruption("malformed page image record in " + wal_path.string());
        }
        page_id_t page_id = 0;
        std::memcpy(&page_id, payload.data() + sizeof(PAGE_IMAGE_TYPE), sizeof(page_id));
        run.emplace_back(page_id, std::move(payload));
        break;
      }
      case COMMIT_TYPE: {
        if (payload_length != COMMIT_PAYLOAD_SIZE) {
          return Status::Corruption("malformed commit record in " + wal_path.string());
        }
        if (!db_fd.Valid()) {
          db_fd = UniqueFd(::open(db_path.c_str(), O_RDWR | O_CLOEXEC));
          if (!db_fd.Valid()) {
            return ErrnoStatus("open");
          }
        }
        // Full page images make this blind and idempotent: overwrite each
        // page with its post-image, no questions asked.
        for (const auto &[page_id, image] : run) {
          if (auto status =
                  FullPwrite(db_fd.Get(), image.data() + PAGE_IMAGE_DATA_OFFSET, PAGE_SIZE, page_id * PAGE_SIZE);
              !status.Ok()) {
            return status;
          }
        }
        run.clear();
        applied = true;
        break;
      }
      default:
        return Status::Corruption("unrecognized record in " + wal_path.string());
    }
  }
  // A trailing run with no commit record is an operation that never made it
  // to its durability point; `run` drops it here.

  // Order matters: the database must be durable before the log forgets the
  // images that could rebuild it.
  if (applied && ::fsync(db_fd.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  if (wal_size > sizeof(WAL_MAGIC)) {
    if (::ftruncate(wal_fd.Get(), static_cast<off_t>(sizeof(WAL_MAGIC))) < 0) {
      return ErrnoStatus("ftruncate");
    }
    if (::fsync(wal_fd.Get()) < 0) {
      return ErrnoStatus("fsync");
    }
  }
  return {};
}

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
    if (auto status = FullPwrite(wal.fd_.Get(), &WAL_MAGIC, sizeof(WAL_MAGIC), 0); !status.Ok()) {
      return std::unexpected(std::move(status));
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
  const auto magic_read = FullPread(wal.fd_.Get(), &magic, sizeof(magic), 0);
  if (!magic_read) {
    return std::unexpected(magic_read.error());
  }
  if (*magic_read != sizeof(magic) || magic != WAL_MAGIC) {
    return std::unexpected(Status::InvalidArgument("not a TinyDB write-ahead log: " + wal_path.string()));
  }
  return wal;
}

void Wal::AppendPageImage(page_id_t page_id, const char *data) {
  std::array<char, PAGE_IMAGE_PAYLOAD_SIZE> payload{};
  payload[0] = PAGE_IMAGE_TYPE;
  std::memcpy(payload.data() + sizeof(PAGE_IMAGE_TYPE), &page_id, sizeof(page_id));
  std::memcpy(payload.data() + PAGE_IMAGE_DATA_OFFSET, data, PAGE_SIZE);
  AppendRecord(pending_, payload.data(), PAGE_IMAGE_PAYLOAD_SIZE);
}

void Wal::DiscardPending() { pending_.clear(); }

auto Wal::Commit() -> Status {
  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  TINYDB_CHECK(!pending_.empty(), "committing an operation that logged no page images");

  constexpr char commit_payload = COMMIT_TYPE;
  AppendRecord(pending_, &commit_payload, COMMIT_PAYLOAD_SIZE);

  // One write, one fsync: the whole run reaches the file together, and a
  // crash anywhere before the fsync returns leaves at worst a torn tail for
  // the recovery scan to discard.
  if (auto status = FullPwrite(fd_.Get(), pending_.data(), pending_.size(), size_bytes_); !status.Ok()) {
    return status;
  }
  if (::fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  size_bytes_ += pending_.size();
  pending_.clear();
  return {};
}

auto Wal::SizeBytes() const -> std::uint64_t { return size_bytes_; }

auto Wal::Reset() -> Status {
  TINYDB_CHECK(fd_.Valid(), "resetting a moved-from log");
  TINYDB_CHECK(pending_.empty(), "resetting the log mid-operation");

  if (::ftruncate(fd_.Get(), static_cast<off_t>(sizeof(WAL_MAGIC))) < 0) {
    return ErrnoStatus("ftruncate");
  }
  // Make the truncation durable before new records land where old ones were.
  // Without this, a crash could leave stale pre-checkpoint records beyond
  // the new tail — and a stale record starting exactly where the new tail
  // ends still passes its CRC, so recovery would replay an old page image
  // over newer committed data.
  if (::fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  size_bytes_ = sizeof(WAL_MAGIC);
  return {};
}

}  // namespace tinydb
