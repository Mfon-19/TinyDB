#include <tinydb/check.h>
#include <tinydb/file_header.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"

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

// A new file's data can be durable while its directory entry is not. Commit
// cannot claim durability for a freshly created WAL until the parent
// directory is synced too.
auto SyncParentDirectory(const std::filesystem::path &path) -> Status {
  auto parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }

  auto dir_fd = UniqueFd(io::Open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!dir_fd.Valid()) {
    return ErrnoStatus("open directory");
  }
  if (io::Fsync(dir_fd.Get()) < 0) {
    return ErrnoStatus("fsync directory");
  }
  return {};
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
    const auto result = io::Pwrite(fd, bytes + written, size - written, offset + written);
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
    const auto result = io::Pread(fd, bytes + total, size - total, offset + total);
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

// Recovery may rewrite the database header, so DiskManager::Open cannot run
// first. This preflight still rejects the dangerous case: a non-empty file
// that is not already a TinyDB database must not be overwritten by a WAL whose
// name merely happens to match.
auto ValidateDatabaseFileForRecovery(const std::filesystem::path &db_path) -> Status {
  auto db_fd = UniqueFd(io::Open(db_path, O_RDONLY | O_CLOEXEC));
  if (!db_fd.Valid()) {
    if (errno == ENOENT) {
      return {};
    }
    return ErrnoStatus("open");
  }

  struct stat stat_buffer {};
  if (io::Fstat(db_fd.Get(), &stat_buffer) < 0) {
    return ErrnoStatus("fstat");
  }
  if (stat_buffer.st_size == 0) {
    return {};
  }

  FileHeader header{};
  const auto header_read = FullPread(db_fd.Get(), &header, sizeof(header), 0);
  if (!header_read) {
    return header_read.error();
  }
  if (*header_read != sizeof(header) || header.magic != TINYDB_FILE_MAGIC || header.page_size != PAGE_SIZE) {
    return Status::InvalidArgument("not a TinyDB database file: " + db_path.string());
  }
  return {};
}

}  // namespace

auto Wal::PathFor(const std::filesystem::path &db_path) -> std::filesystem::path {
  auto wal_path = db_path;
  wal_path += "-wal";  // concatenation ("data.db-wal"), not a new component
  return wal_path;
}

auto Wal::Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status {
  /*
    Recovery is redo-only. The WAL is the source of truth for operations that
    reached their durability point but may not have reached the database file.

      WAL bytes:

        +---------+--------------------+--------------------+------------+
        | "TDBW"  | op 1               | op 2               | torn tail  |
        +---------+--------------------+--------------------+------------+
                  | PAGE* COMMIT       | PAGE* COMMIT       | PAGE* ...  |
                  +--------- durable --+--------- durable --+-- ignored -+

      One operation:

        PAGE_IMAGE(page 7, post-image)
        PAGE_IMAGE(page 0, post-image)
        COMMIT

      Invariants:

        1. A run with no COMMIT did not commit. Drop it.
        2. A bad frame length or CRC is treated as the start of a torn tail.
           Stop scanning there; do not trust later bytes.
        3. A well-formed but semantically impossible record is corruption,
           not a torn write. Refuse recovery.
        4. Replaying full-page post-images is idempotent:

              before: page P = old
              replay: page P = image
              replay again: page P = image

        5. Never truncate the WAL until the database file, and the containing
           directory if the DB was recreated, are durable.

      Ordering at the end:

        replay committed images -> fsync(db) -> fsync(db parent if created)
                                -> truncate WAL -> fsync(WAL)

      If a crash happens before the WAL truncation is durable, recovery simply
      replays the same committed images again.
  */

  // Step 1: open the WAL. A missing WAL means the database file is already
  // the best available state.
  auto wal_fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CLOEXEC));
  if (!wal_fd.Valid()) {
    if (errno == ENOENT) {
      return {};  // no log, nothing to recover
    }
    return ErrnoStatus("open");
  }

  struct stat stat_buffer {};
  if (io::Fstat(wal_fd.Get(), &stat_buffer) < 0) {
    return ErrnoStatus("fstat");
  }
  const auto wal_size = static_cast<std::uint64_t>(stat_buffer.st_size);

  // Step 2: prove that replay is allowed to touch the base file. Recovery can
  // rewrite page 0, so DiskManager::Open cannot validate first; this preflight
  // rejects the dangerous "foreign.db + valid foreign.db-wal" pairing before
  // any WAL image can overwrite user data.
  if (auto status = ValidateDatabaseFileForRecovery(db_path); !status.Ok()) {
    return status;
  }

  // A crash can beat the header to disk: Open() creates the file and only
  // then writes and fsyncs the magic. Clear the remnant so the next Open()
  // stamps it afresh.
  if (wal_size < sizeof(WAL_MAGIC)) {
    if (wal_size > 0 && io::Ftruncate(wal_fd.Get(), 0) < 0) {
      return ErrnoStatus("ftruncate");
    }
    return {};
  }

  // Step 3: verify the WAL header. From here on, every accepted record still
  // needs its own CRC check; the magic only proves this is the right file type.
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
  bool created_db_file = false;
  std::vector<std::pair<page_id_t, std::vector<char>>> run;
  bool applied = false;
  auto offset = static_cast<std::uint64_t>(sizeof(WAL_MAGIC));

  while (offset < wal_size) {
    // Step 4: read one framed payload.
    //
    //   +----------------+-------------+----------------------+
    //   | payload_length | payload_crc | payload bytes        |
    //   +----------------+-------------+----------------------+
    //
    // Short frame, impossible length, short payload, or bad CRC means the
    // writer crashed before this record was durable. The current and later
    // bytes are ignored by breaking out of the scan.
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
        // Step 5a: accumulate this operation's post-images in memory. They
        // are not applied until a later COMMIT proves the whole run durable.
        if (payload_length != PAGE_IMAGE_PAYLOAD_SIZE) {
          return Status::Corruption("malformed page image record in " + wal_path.string());
        }
        page_id_t page_id = 0;
        std::memcpy(&page_id, payload.data() + sizeof(PAGE_IMAGE_TYPE), sizeof(page_id));
        run.emplace_back(page_id, std::move(payload));
        break;
      }
      case COMMIT_TYPE: {
        // Step 5b: COMMIT closes the current run. Every buffered page image
        // before it is now durable and must be replayed.
        //
        //   run buffer: [page 3 image][page 8 image] + COMMIT
        //                    |             |
        //                    v             v
        //   database:    overwrite p3  overwrite p8
        if (payload_length != COMMIT_PAYLOAD_SIZE) {
          return Status::Corruption("malformed commit record in " + wal_path.string());
        }
        if (!db_fd.Valid()) {
          db_fd = UniqueFd(io::Open(db_path, O_RDWR | O_CLOEXEC));
          if (!db_fd.Valid() && errno == ENOENT) {
            db_fd = UniqueFd(io::Open(db_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
            created_db_file = db_fd.Valid();
          }
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

  // Step 6: make replay durable. The database must be on stable storage
  // before the log forgets the images that could rebuild it.
  if (applied && io::Fsync(db_fd.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  if (applied && created_db_file) {
    if (auto status = SyncParentDirectory(db_path); !status.Ok()) {
      return status;
    }
  }

  // Step 7: forget only the images that are now safely in the database file.
  //
  //   before reset: ["TDBW"][committed run][committed run][ignored tail]
  //   after reset:  ["TDBW"]
  //
  // fsync(WAL) makes the truncation durable. If the crash happens before that
  // fsync returns, the old committed runs may still be present, which is safe
  // because replaying full-page images is idempotent.
  if (wal_size > sizeof(WAL_MAGIC)) {
    if (io::Ftruncate(wal_fd.Get(), sizeof(WAL_MAGIC)) < 0) {
      return ErrnoStatus("ftruncate");
    }
    if (io::Fsync(wal_fd.Get()) < 0) {
      return ErrnoStatus("fsync");
    }
  }
  return {};
}

auto Wal::Open(const std::filesystem::path &wal_path) -> Result<Wal> {
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));

  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }

  struct stat stat_buffer {};
  if (io::Fstat(fd.Get(), &stat_buffer) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }

  Wal wal(std::move(fd), static_cast<std::uint64_t>(stat_buffer.st_size));

  if (stat_buffer.st_size == 0) {
    // Fresh log: stamp the magic now and make it durable, so every later
    // open can tell a TinyDB log from an arbitrary file.
    if (auto status = FullPwrite(wal.fd_.Get(), &WAL_MAGIC, sizeof(WAL_MAGIC), 0); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    if (io::Fsync(wal.fd_.Get()) < 0) {
      return std::unexpected(ErrnoStatus("fsync"));
    }
    if (auto status = SyncParentDirectory(wal_path); !status.Ok()) {
      return std::unexpected(std::move(status));
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
  /*
    Commit is the only durability point for one StorageEngine operation.

      pending_ before Commit():

        +------------------+------------------+
        | PAGE_IMAGE p0    | PAGE_IMAGE p9    |
        +------------------+------------------+

      Commit appends one final record in memory:

        +------------------+------------------+----------+
        | PAGE_IMAGE p0    | PAGE_IMAGE p9    | COMMIT   |
        +------------------+------------------+----------+

      Then it appends that whole byte run to the existing WAL:

        WAL before:
          +---------+----------------------+----------------------+
          | "TDBW"  | old committed run    | old committed run    |
          +---------+----------------------+----------------------+
                                                       size_bytes_

        WAL after FullPwrite, before fsync:
          +---------+----------------------+----------------------+----------------------+
          | "TDBW"  | old committed run    | old committed run    | new PAGE* COMMIT    |
          +---------+----------------------+----------------------+----------------------+

      Invariants:

        1. Page images are post-images of every page dirtied by the operation.
        2. No dirty frame from the operation may be written back by the buffer
           pool before this Commit succeeds. That is the no-steal half of the
           redo-only design.
        3. Until fsync(fd_) returns, recovery may see no new bytes or a torn
           tail. Both are safe: no complete COMMIT means no replay.
        4. After fsync(fd_) returns, every PAGE_IMAGE before the COMMIT is
           durable, so the operation is committed even if the database file
           still has old page contents.
        5. size_bytes_ advances only after fsync succeeds. If fsync fails, the
           caller poisons the engine and recovery will decide what prefix is
           valid by scanning lengths and CRCs.
  */

  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  TINYDB_CHECK(!pending_.empty(), "committing an operation that logged no page images");

  // Step 1: close the in-memory run with a COMMIT record. Recovery replays a
  // run only when this marker is present and its frame passes CRC validation.
  constexpr char commit_payload = COMMIT_TYPE;
  AppendRecord(pending_, &commit_payload, COMMIT_PAYLOAD_SIZE);

  // Step 2: append the entire operation as one contiguous byte range.
  //
  // A single pwrite is not an atomicity requirement; FullPwrite may loop after
  // short writes. The framing is what gives recovery a clean prefix: if a crash
  // interrupts the append, the last frame is short or fails its CRC.
  if (auto status = FullPwrite(fd_.Get(), pending_.data(), pending_.size(), size_bytes_); !status.Ok()) {
    return status;
  }

  // Step 3: fsync is the durability point. Returning Ok before this would let
  // the caller acknowledge a write that can disappear on power loss.
  if (io::Fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }

  // Step 4: publish the new logical WAL tail only after the durable append.
  // The pending buffer is now represented on disk and can be reused.
  size_bytes_ += pending_.size();
  pending_.clear();
  return {};
}

auto Wal::SizeBytes() const -> std::uint64_t { return size_bytes_; }

auto Wal::Reset() -> Status {
  TINYDB_CHECK(fd_.Valid(), "resetting a moved-from log");
  TINYDB_CHECK(pending_.empty(), "resetting the log mid-operation");

  if (io::Ftruncate(fd_.Get(), sizeof(WAL_MAGIC)) < 0) {
    return ErrnoStatus("ftruncate");
  }
  // Make the truncation durable before new records land where old ones were.
  // Without this, a crash could leave stale pre-checkpoint records beyond
  // the new tail — and a stale record starting exactly where the new tail
  // ends still passes its CRC, so recovery would replay an old page image
  // over newer committed data.
  if (io::Fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  size_bytes_ = sizeof(WAL_MAGIC);
  return {};
}

}  // namespace tinydb
