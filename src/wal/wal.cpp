#include <tinydb/check.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "util/crc32.h"
#include "wal/wal_codec.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

// PAGE_IMAGE payload: [page ID u64][exact 4096-byte encoded page]. The page
// retains its own checksum and identity in addition to the WAL record CRC.
constexpr std::size_t PAGE_IMAGE_PAGE_ID_OFFSET = 0;
constexpr std::size_t PAGE_IMAGE_DATA_OFFSET = sizeof(page_id_t);
constexpr std::size_t PAGE_IMAGE_PAYLOAD_BYTES = PAGE_IMAGE_DATA_OFFSET + PAGE_SIZE;
// COMMIT payload: [image count u32][CRC of encoded image records u32]
//                 [commit-record LSN u64].
// These redundant bindings prevent a valid-looking commit record from
// accepting a run with a missing, duplicated, or reordered middle image.
constexpr std::size_t COMMIT_IMAGE_COUNT_OFFSET = 0;
constexpr std::size_t COMMIT_DIGEST_OFFSET = 4;
constexpr std::size_t COMMIT_LSN_OFFSET = 8;
constexpr std::size_t COMMIT_PAYLOAD_BYTES = 16;
constexpr std::size_t MAX_RECORD_BYTES = wal_format::RECORD_HEADER_BYTES + PAGE_IMAGE_PAYLOAD_BYTES;

auto ErrnoStatus(std::string_view operation) -> Status {
  return Status::IoError(std::string(operation) + ": " + std::generic_category().message(errno));
}

auto SyncParentDirectory(const std::filesystem::path &path) -> Status {
  // Required only when recovery or WAL creation creates a new directory entry;
  // fsyncing the file descriptor alone does not make that name durable.
  auto parent = path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  auto directory = UniqueFd(io::Open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory.Valid()) {
    return ErrnoStatus("open directory");
  }
  if (io::Fsync(directory.Get()) < 0) {
    return ErrnoStatus("fsync directory");
  }
  return {};
}

auto FullPwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> Status {
  // WAL appends must tolerate both EINTR and short writes. A partial record is
  // safe on disk because recovery treats an incomplete trailing frame as a
  // torn tail, but the caller still receives the I/O failure.
  const auto *bytes = static_cast<const char *>(data);
  auto written = std::size_t{0};
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

auto FullPread(int fd, void *data, std::size_t size, std::uint64_t offset) -> Result<std::size_t> {
  // Returns the number actually obtained so recovery can distinguish clean EOF
  // at a torn tail from an environmental read error.
  auto *bytes = static_cast<char *>(data);
  auto total = std::size_t{0};
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

auto Bytes(const std::vector<char> &value) -> std::span<const std::byte> { return std::as_bytes(std::span{value}); }

auto ReadDatabaseUuidForRecovery(const std::filesystem::path &db_path) -> Result<std::optional<DatabaseUuid>> {
  // Recovery must establish database/WAL identity before replay, but the
  // database may be missing or its superblocks may be exactly the pages the
  // WAL needs to restore. optional<UUID> represents that recoverable absence.
  auto fd = UniqueFd(io::Open(db_path, O_RDONLY | O_CLOEXEC));
  if (!fd.Valid()) {
    if (errno == ENOENT) {
      return std::optional<DatabaseUuid>{};
    }
    return std::unexpected(ErrnoStatus("open"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  if (file_stat.st_size == 0) {
    return std::optional<DatabaseUuid>{};
  }

  auto prefix = std::vector<char>(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(file_stat.st_size), FIRST_DATA_PAGE_ID * PAGE_SIZE));
  const auto read = FullPread(fd.Get(), prefix.data(), prefix.size(), 0);
  if (!read) {
    return std::unexpected(read.error());
  }
  if (std::ranges::all_of(prefix, [](char byte) { return byte == 0; })) {
    // Creation may have reserved zero pages before a superblock became durable.
    return std::optional<DatabaseUuid>{};
  }
  if (prefix.size() < FIRST_DATA_PAGE_ID * PAGE_SIZE) {
    return std::unexpected(Status::UnsupportedFormat("database does not contain TinyDB dual superblocks"));
  }

  const auto all = std::as_bytes(std::span{prefix});
  const auto page_a = all.first(PAGE_SIZE);
  const auto page_b = all.subspan(PAGE_SIZE, PAGE_SIZE);
  const auto selected = storage::SelectSuperblock(page_a, page_b);
  if (selected) {
    return std::optional{selected->value.database_uuid};
  }

  const auto recognized = [&](std::span<const std::byte> page) {
    return std::ranges::equal(storage::SUPERBLOCK_MAGIC, page.first(storage::SUPERBLOCK_MAGIC.size()));
  };
  if (recognized(page_a) || recognized(page_b)) {
    // A recognized but damaged superblock can be repaired from a committed WAL
    // image. Its UUID cannot be trusted yet, so defer identity to that WAL.
    return std::optional<DatabaseUuid>{};
  }
  return std::unexpected(selected.error());
}

auto ValidatePageImage(page_id_t page_id, std::span<const std::byte> page) -> Status {
  // WAL CRC proves the record arrived intact; page validation additionally
  // proves the payload is a semantically valid page for the target file offset.
  if (page_id == SUPERBLOCK_A_PAGE_ID || page_id == SUPERBLOCK_B_PAGE_ID) {
    const auto decoded = storage::DecodeSuperblock(page);
    return decoded ? Status{} : decoded.error();
  }
  const auto decoded = storage::DecodeDataPageHeader(page, page_id);
  return decoded ? Status{} : decoded.error();
}

}  // namespace

auto Wal::PathFor(const std::filesystem::path &db_path) -> std::filesystem::path {
  auto wal_path = db_path;
  wal_path += "-wal";
  return wal_path;
}

auto Wal::Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status {
  // Recovery is physical and idempotent: applying the same final page images
  // repeatedly produces the same database bytes. No B+ tree object or logical
  // mutation code participates here.
  auto wal_fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CLOEXEC));
  if (!wal_fd.Valid()) {
    return errno == ENOENT ? Status{} : ErrnoStatus("open");
  }
  struct stat wal_stat {};
  if (io::Fstat(wal_fd.Get(), &wal_stat) < 0) {
    return ErrnoStatus("fstat");
  }
  const auto wal_size = static_cast<std::uint64_t>(wal_stat.st_size);
  if (wal_size == 0) {
    return {};
  }

  if (wal_size < wal_format::HEADER_BYTES) {
    // A crash during first-time header creation can leave a prefix of the
    // current magic or a zero-filled file. Neither could contain a committed
    // record, so it is safe to clear. Arbitrary foreign bytes are preserved and
    // rejected instead of being silently destroyed.
    auto partial = std::vector<char>(wal_size);
    const auto read = FullPread(wal_fd.Get(), partial.data(), partial.size(), 0);
    if (!read) {
      return read.error();
    }
    const auto bytes = std::as_bytes(std::span{partial});
    const auto prefix_bytes = std::min(bytes.size(), wal_format::MAGIC.size());
    const bool magic_prefix =
        std::ranges::equal(bytes.first(prefix_bytes), std::span{wal_format::MAGIC}.first(prefix_bytes));
    const bool zero = std::ranges::all_of(bytes, [](std::byte byte) { return byte == std::byte{0}; });
    if (!magic_prefix && !zero) {
      return Status::UnsupportedFormat("unrecognized TinyDB WAL prefix");
    }
    if (io::Ftruncate(wal_fd.Get(), 0) < 0) {
      return ErrnoStatus("ftruncate");
    }
    return {};
  }

  auto encoded_header = std::vector<char>(wal_format::HEADER_BYTES);
  const auto header_read = FullPread(wal_fd.Get(), encoded_header.data(), encoded_header.size(), 0);
  if (!header_read) {
    return header_read.error();
  }
  const auto header = wal_format::DecodeHeader(Bytes(encoded_header));
  if (!header) {
    return header.error();
  }

  const auto database_uuid = ReadDatabaseUuidForRecovery(db_path);
  if (!database_uuid) {
    return database_uuid.error();
  }
  if (database_uuid->has_value() && **database_uuid != header->database_uuid) {
    // Refuse before opening the database writable or applying a single page.
    return Status::InvalidArgument("write-ahead log does not belong to this database: " + wal_path.string());
  }

  // `run` contains decoded page images for the transaction currently being
  // scanned. Nothing is written until its COMMIT record validates all of them.
  auto db_fd = UniqueFd{};
  auto run = std::vector<std::pair<page_id_t, std::array<char, PAGE_SIZE>>>{};
  auto run_bytes = std::vector<char>{};
  auto current_transaction = std::uint64_t{0};
  auto applied = false;
  auto created_db_file = false;
  auto offset = static_cast<std::uint64_t>(wal_format::HEADER_BYTES);

  while (offset < wal_size) {
    // Read only the total-length prefix first. This bounds allocation before a
    // corrupt record can make recovery allocate an arbitrary amount of memory.
    auto total_prefix = std::array<std::byte, sizeof(std::uint32_t)>{};
    const auto prefix_read = FullPread(wal_fd.Get(), total_prefix.data(), total_prefix.size(), offset);
    if (!prefix_read) {
      return prefix_read.error();
    }
    const auto total = storage::GetLittleEndian<std::uint32_t>(total_prefix, 0);
    if (*prefix_read != total_prefix.size() || !total || *total < wal_format::RECORD_HEADER_BYTES ||
        *total > MAX_RECORD_BYTES || offset + *total > wal_size) {
      // An incomplete/invalid frame at the physical tail is an uncommitted torn
      // suffix. Since no COMMIT has accepted it, the committed prefix is enough.
      break;
    }

    auto encoded_record = std::vector<char>(*total);
    const auto record_read = FullPread(wal_fd.Get(), encoded_record.data(), encoded_record.size(), offset);
    if (!record_read) {
      return record_read.error();
    }
    if (*record_read != encoded_record.size()) {
      break;
    }
    const auto record = wal_format::DecodeRecord(Bytes(encoded_record));
    if (!record) {
      // A checksum mismatch is tolerable only when this record reaches exact
      // EOF: that is the shape of a torn final write. The same mismatch in the
      // durable middle would hide later records and is reported as corruption.
      const bool checksum_torn_at_tail =
          record.error().Message() == "WAL record checksum mismatch" && offset + encoded_record.size() == wal_size;
      if (checksum_torn_at_tail) {
        break;
      }
      return record.error();
    }
    if (record->lsn != offset) {
      // LSN-to-position binding detects spliced/reordered records even if each
      // individual record carries a valid checksum.
      return Status::Corruption("WAL record LSN does not match its position");
    }

    if (record->type == wal_format::RecordType::PageImage) {
      if (record->payload.size() != PAGE_IMAGE_PAYLOAD_BYTES) {
        return Status::Corruption("malformed WAL page-image payload");
      }
      if (current_transaction == 0) {
        // The first image opens a run; subsequent images and the commit must
        // carry the same transaction ID. Interleaving is not supported.
        current_transaction = record->transaction_id;
      }
      if (record->transaction_id != current_transaction) {
        return Status::Corruption("interleaved WAL transactions");
      }
      const auto page_id = storage::GetLittleEndian<page_id_t>(record->payload, PAGE_IMAGE_PAGE_ID_OFFSET);
      if (!page_id) {
        return Status::Corruption("truncated WAL page ID");
      }
      auto image = std::array<char, PAGE_SIZE>{};
      std::memcpy(image.data(), record->payload.data() + PAGE_IMAGE_DATA_OFFSET, PAGE_SIZE);
      if (auto status = ValidatePageImage(*page_id, std::as_bytes(std::span{image})); !status.Ok()) {
        return status;
      }
      run.emplace_back(*page_id, image);
      // Digest the exact encoded records, not merely decoded page bytes. The
      // commit therefore binds framing metadata as well as image contents.
      run_bytes.insert(run_bytes.end(), encoded_record.begin(), encoded_record.end());
    } else {
      if (record->payload.size() != COMMIT_PAYLOAD_BYTES || run.empty() ||
          record->transaction_id != current_transaction) {
        return Status::Corruption("malformed WAL commit record");
      }
      const auto image_count = storage::GetLittleEndian<std::uint32_t>(record->payload, COMMIT_IMAGE_COUNT_OFFSET);
      const auto digest = storage::GetLittleEndian<std::uint32_t>(record->payload, COMMIT_DIGEST_OFFSET);
      const auto commit_lsn = storage::GetLittleEndian<std::uint64_t>(record->payload, COMMIT_LSN_OFFSET);
      if (!image_count || !digest || !commit_lsn || *image_count != run.size() ||
          *digest != Crc32(std::as_bytes(std::span{run_bytes})) || *commit_lsn != offset) {
        return Status::Corruption("WAL commit does not bind its page images");
      }

      // Delay opening/creating the database until a complete transaction is
      // proven committed. A WAL containing only a torn run must not create or
      // mutate the database file.
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
      for (const auto &[page_id, image] : run) {
        // Full physical redo may extend the file. Reapplying after a recovery
        // crash simply overwrites the same offsets with the same bytes.
        if (auto status = FullPwrite(db_fd.Get(), image.data(), image.size(), page_id * PAGE_SIZE); !status.Ok()) {
          return status;
        }
      }
      run.clear();
      run_bytes.clear();
      current_transaction = 0;
      applied = true;
    }
    offset += encoded_record.size();
  }

  if (applied && io::Fsync(db_fd.Get()) < 0) {
    // The WAL remains untouched on failure, so reopening retries all commits.
    return ErrnoStatus("fsync");
  }
  if (applied && created_db_file) {
    // Only recovery that created the path must make its directory entry durable.
    if (auto status = SyncParentDirectory(db_path); !status.Ok()) {
      return status;
    }
  }
  if (wal_size > wal_format::HEADER_BYTES) {
    // Truncation is last: committed redo may be forgotten only after database
    // pages (and a newly created directory entry) are durable.
    if (io::Ftruncate(wal_fd.Get(), wal_format::HEADER_BYTES) < 0) {
      return ErrnoStatus("ftruncate");
    }
    if (io::Fsync(wal_fd.Get()) < 0) {
      return ErrnoStatus("fsync");
    }
  }
  return {};
}

auto Wal::Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid) -> Result<Wal> {
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  Wal wal(std::move(fd), static_cast<std::uint64_t>(file_stat.st_size));

  if (file_stat.st_size == 0) {
    // Fresh WAL durability ordering is contents -> file fsync -> directory
    // fsync. A crash before completion leaves the short-header case Recover
    // handles without pretending any transaction committed.
    const auto encoded = wal_format::EncodeHeader(wal_format::Header{.database_uuid = database_uuid});
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    if (auto status = FullPwrite(wal.fd_.Get(), encoded->data(), encoded->size(), 0); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    if (io::Fsync(wal.fd_.Get()) < 0) {
      return std::unexpected(ErrnoStatus("fsync"));
    }
    if (auto status = SyncParentDirectory(wal_path); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    wal.size_bytes_ = wal_format::HEADER_BYTES;
    return wal;
  }

  if (file_stat.st_size != static_cast<off_t>(wal_format::HEADER_BYTES)) {
    // Normal Open accepts only a clean header-only log. Any records must first
    // pass through Recover, which is the sole parser allowed to replay/truncate.
    auto prefix = std::array<std::byte, wal_format::MAGIC.size()>{};
    const auto prefix_size = std::min<std::size_t>(prefix.size(), static_cast<std::size_t>(file_stat.st_size));
    const auto read = FullPread(wal.fd_.Get(), prefix.data(), prefix_size, 0);
    if (!read) {
      return std::unexpected(read.error());
    }
    if (prefix_size < wal_format::MAGIC.size() || !std::ranges::equal(prefix, wal_format::MAGIC)) {
      return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB WAL format"));
    }
    return std::unexpected(Status::Corruption("WAL was not recovered before open"));
  }
  auto encoded = std::vector<char>(wal_format::HEADER_BYTES);
  const auto read = FullPread(wal.fd_.Get(), encoded.data(), encoded.size(), 0);
  if (!read) {
    return std::unexpected(read.error());
  }
  const auto header = wal_format::DecodeHeader(Bytes(encoded));
  if (!header) {
    return std::unexpected(header.error());
  }
  if (header->database_uuid != database_uuid) {
    return std::unexpected(
        Status::InvalidArgument("write-ahead log does not belong to this database: " + wal_path.string()));
  }
  return wal;
}

void Wal::AppendPageImage(page_id_t page_id, const char *data) {
  // This path cannot fail for valid engine-owned page bytes: buffer sizing and
  // field offsets are fixed. A failure indicates an internal invariant bug,
  // hence TINYDB_CHECK rather than an environmental Status.
  auto payload = std::vector<std::byte>(PAGE_IMAGE_PAYLOAD_BYTES);
  TINYDB_CHECK(storage::PutLittleEndian(payload, PAGE_IMAGE_PAGE_ID_OFFSET, page_id) &&
                   storage::PutBytes(payload, PAGE_IMAGE_DATA_OFFSET, std::as_bytes(std::span{data, PAGE_SIZE})),
               "failed to encode WAL page image");
  const auto record = wal_format::EncodeRecord(wal_format::RecordType::PageImage, next_transaction_id_,
                                               size_bytes_ + pending_.size(), payload);
  TINYDB_CHECK(record.has_value(), "failed to encode WAL page-image record");
  pending_.insert(pending_.end(), record->begin(), record->end());
  ++pending_image_count_;
}

void Wal::DiscardPending() {
  // Durable size and transaction ID do not advance. A retry/reopen therefore
  // cannot mistake abandoned memory records for a committed transaction.
  pending_.clear();
  pending_image_count_ = 0;
}

auto Wal::Commit() -> Status {
  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  TINYDB_CHECK(!pending_.empty() && pending_image_count_ > 0, "committing an operation that logged no page images");

  // The commit's LSN is known before encoding because it begins immediately
  // after the already encoded page-image run.
  const auto commit_lsn = size_bytes_ + pending_.size();
  auto payload = std::array<std::byte, COMMIT_PAYLOAD_BYTES>{};
  TINYDB_CHECK(
      storage::PutLittleEndian(payload, COMMIT_IMAGE_COUNT_OFFSET, static_cast<std::uint32_t>(pending_image_count_)) &&
          storage::PutLittleEndian(payload, COMMIT_DIGEST_OFFSET, Crc32(std::as_bytes(std::span{pending_}))) &&
          storage::PutLittleEndian(payload, COMMIT_LSN_OFFSET, commit_lsn),
      "failed to encode WAL commit payload");
  const auto commit =
      wal_format::EncodeRecord(wal_format::RecordType::Commit, next_transaction_id_, commit_lsn, payload);
  TINYDB_CHECK(commit.has_value(), "failed to encode WAL commit record");
  pending_.insert(pending_.end(), commit->begin(), commit->end());

  // One contiguous append preserves run ordering; the following fsync is the
  // exact point after which StorageEngine may acknowledge durability.
  if (auto status = FullPwrite(fd_.Get(), pending_.data(), pending_.size(), size_bytes_); !status.Ok()) {
    return status;
  }
  if (io::Fsync(fd_.Get()) < 0) {
    // Do not advance size_bytes_ or clear pending_: the outcome is not treated
    // as acknowledged, and the owning engine poisons itself before more work.
    return ErrnoStatus("fsync");
  }
  size_bytes_ += pending_.size();
  pending_.clear();
  pending_image_count_ = 0;
  ++next_transaction_id_;
  return {};
}

auto Wal::SizeBytes() const -> std::uint64_t { return size_bytes_; }

auto Wal::Reset() -> Status {
  TINYDB_CHECK(fd_.Valid(), "resetting a moved-from log");
  TINYDB_CHECK(pending_.empty(), "resetting the log mid-operation");
  // Preserve the UUID/version header so subsequent commits can append without
  // recreating the file or its directory entry.
  if (io::Ftruncate(fd_.Get(), wal_format::HEADER_BYTES) < 0) {
    return ErrnoStatus("ftruncate");
  }
  if (io::Fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  size_bytes_ = wal_format::HEADER_BYTES;
  return {};
}

}  // namespace tinydb
