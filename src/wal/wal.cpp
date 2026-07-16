#include <tinydb/check.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "txn/database_state.h"
#include "wal/wal_codec.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
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

/*
** CURRENT WAL PROTOCOL
**
** The writer collects final data pages in memory, adds the resulting logical
** database state, and appends the complete run with its binding Commit record
** before calling fsync. That fsync is the durability point. StorageEngine may
** publish only after it succeeds. On append or sync failure the durable tail
** is not advanced in memory and the engine stops accepting work until reopen.
**
** Recovery scans complete records from the header. It retains one transaction
** run in memory and writes no database page until the Commit record validates
** the run. A malformed record in the durable middle is corruption; an
** incomplete final run is an uncommitted torn tail. Redo writes physical page
** images, fsyncs the database, and only then truncates the WAL back to its
** header. Repeating recovery after a crash is therefore idempotent.
*/

constexpr std::size_t MAX_RECORD_BYTES = wal_format::RECORD_HEADER_BYTES + wal_format::PAGE_IMAGE_PAYLOAD_BYTES;

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

struct SegmentFile {
  std::filesystem::path path;
  std::uint64_t segment_id{0};
  bool active{false};
};

auto DiscoverSegmentFiles(const std::filesystem::path &wal_path) -> Result<std::vector<SegmentFile>> {
  auto parent = wal_path.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const auto active_name = wal_path.filename().string();
  const auto archive_prefix = active_name + ".";
  constexpr auto archive_suffix = std::string_view{".segment"};
  auto files = std::vector<SegmentFile>{};
  auto error = std::error_code{};
  auto iterator = std::filesystem::directory_iterator(parent, error);
  if (error) {
    return std::unexpected(Status::IoError("enumerate WAL segments: " + error.message()));
  }
  for (const auto &entry : iterator) {
    const auto name = entry.path().filename().string();
    if (name == active_name) {
      files.push_back(SegmentFile{.path = entry.path(), .active = true});
      continue;
    }
    if (!name.starts_with(archive_prefix) || !name.ends_with(archive_suffix)) {
      continue;
    }
    const auto digits = std::string_view{name}.substr(archive_prefix.size(),
                                                      name.size() - archive_prefix.size() - archive_suffix.size());
    auto segment_id = std::uint64_t{0};
    const auto [end, parse_error] = std::from_chars(digits.data(), digits.data() + digits.size(), segment_id);
    if (parse_error != std::errc{} || end != digits.data() + digits.size() || segment_id == 0) {
      continue;
    }
    files.push_back(SegmentFile{.path = entry.path(), .segment_id = segment_id, .active = false});
  }
  return files;
}

auto ReadDatabaseStateForRecovery(const std::filesystem::path &db_path) -> Result<std::optional<storage::Superblock>> {
  // Recovery must establish database/WAL identity before replay, but the
  // database may be missing or both superblocks may be damaged.
  // optional<Superblock> represents that recoverable absence.
  auto fd = UniqueFd(io::Open(db_path, O_RDONLY | O_CLOEXEC));
  if (!fd.Valid()) {
    if (errno == ENOENT) {
      return std::optional<storage::Superblock>{};
    }
    return std::unexpected(ErrnoStatus("open"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  if (file_stat.st_size == 0) {
    return std::optional<storage::Superblock>{};
  }

  auto prefix = std::vector<char>(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(file_stat.st_size), FIRST_DATA_PAGE_ID * PAGE_SIZE));
  const auto read = FullPread(fd.Get(), prefix.data(), prefix.size(), 0);
  if (!read) {
    return std::unexpected(read.error());
  }
  if (std::ranges::all_of(prefix, [](char byte) { return byte == 0; })) {
    // Creation may have reserved zero pages before a superblock became durable.
    return std::optional<storage::Superblock>{};
  }
  if (prefix.size() < FIRST_DATA_PAGE_ID * PAGE_SIZE) {
    return std::unexpected(Status::UnsupportedFormat("database does not contain TinyDB dual superblocks"));
  }

  const auto all = std::as_bytes(std::span{prefix});
  const auto page_a = all.first(PAGE_SIZE);
  const auto page_b = all.subspan(PAGE_SIZE, PAGE_SIZE);
  const auto selected = storage::SelectSuperblock(page_a, page_b);
  if (selected) {
    return std::optional{selected->value};
  }

  const auto recognized = [&](std::span<const std::byte> page) {
    return std::ranges::equal(storage::SUPERBLOCK_MAGIC, page.first(storage::SUPERBLOCK_MAGIC.size()));
  };
  if (recognized(page_a) || recognized(page_b)) {
    // A recognized but damaged superblock can be repaired from a committed WAL
    // image. Its UUID cannot be trusted yet, so defer identity to that WAL.
    return std::optional<storage::Superblock>{};
  }
  return std::unexpected(selected.error());
}

auto EncodeRecoverySuperblock(const wal_format::DecodedTransaction &transaction, const DatabaseUuid &database_uuid,
                              std::uint64_t generation) -> Result<std::array<char, PAGE_SIZE>> {
  const auto &state = transaction.state;
  const auto encoded = storage::EncodeSuperblock(storage::Superblock{
      .database_uuid = database_uuid,
      .generation = generation,
      // Recovery has materialized and will fsync every committed data page, so
      // the recovered visible frontier also becomes the checkpoint frontier.
      .checkpoint_lsn = state.visible_lsn,
      .transaction_id = state.transaction_id,
      .root_page_id = state.root_page_id,
      .allocator_root_page_id = state.allocator_root_page_id,
      .high_water_page_id = state.high_water_page_id,
  });
  if (!encoded) {
    return std::unexpected(encoded.error());
  }
  auto page = std::array<char, PAGE_SIZE>{};
  std::memcpy(page.data(), encoded->data(), encoded->size());
  return page;
}

}  // namespace

auto Wal::PathFor(const std::filesystem::path &db_path) -> std::filesystem::path {
  auto wal_path = db_path;
  wal_path += "-wal";
  return wal_path;
}

auto Wal::SegmentPathFor(const std::filesystem::path &wal_path, std::uint64_t segment_id) -> std::filesystem::path {
  auto path = wal_path;
  path += "." + std::to_string(segment_id) + ".segment";
  return path;
}

auto Wal::Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status {
  /*
  ** SEGMENT DISCOVERY
  **
  ** Archived siblings are immutable. wal_path is the sole active segment and
  ** may contain a torn tail. Headers, IDs, UUIDs, starting LSNs, and record
  ** framing are validated in segment order before any transaction is replayed.
  */
  auto discovered = DiscoverSegmentFiles(wal_path);
  if (!discovered) {
    return discovered.error();
  }
  if (discovered->empty()) {
    return {};
  }

  struct LoadedSegment {
    SegmentFile file;
    wal_format::Header header;
    std::vector<char> bytes;
  };
  auto loaded = std::vector<LoadedSegment>{};
  auto active_partial = false;
  for (auto file : *discovered) {
    auto fd = UniqueFd(io::Open(file.path, O_RDWR | O_CLOEXEC));
    if (!fd.Valid()) {
      return ErrnoStatus("open WAL segment");
    }
    struct stat file_stat {};
    if (io::Fstat(fd.Get(), &file_stat) < 0) {
      return ErrnoStatus("fstat WAL segment");
    }
    const auto size = static_cast<std::uint64_t>(file_stat.st_size);
    auto bytes = std::vector<char>(size);
    const auto read = FullPread(fd.Get(), bytes.data(), bytes.size(), 0);
    if (!read) {
      return read.error();
    }
    if (*read != bytes.size()) {
      return Status::IoError("short read while loading WAL segment");
    }
    if (size < wal_format::HEADER_BYTES) {
      if (!file.active) {
        return Status::Corruption("archived WAL segment has a torn header");
      }
      const auto encoded = std::as_bytes(std::span{bytes});
      const auto prefix_bytes = std::min(encoded.size(), wal_format::MAGIC.size());
      const auto magic_prefix =
          std::ranges::equal(encoded.first(prefix_bytes), std::span{wal_format::MAGIC}.first(prefix_bytes));
      const auto zero = std::ranges::all_of(encoded, [](std::byte byte) { return byte == std::byte{0}; });
      if (!magic_prefix && !zero) {
        return Status::UnsupportedFormat("unrecognized TinyDB WAL prefix");
      }
      active_partial = true;
      continue;
    }
    const auto header = wal_format::DecodeHeader(std::as_bytes(std::span{bytes}).first(wal_format::HEADER_BYTES));
    if (!header) {
      return header.error();
    }
    if (!file.active && file.segment_id != header->segment_id) {
      return Status::Corruption("WAL archive name disagrees with its segment header");
    }
    file.segment_id = header->segment_id;
    loaded.push_back(LoadedSegment{.file = std::move(file), .header = *header, .bytes = std::move(bytes)});
  }
  if (loaded.empty()) {
    // Only an incomplete active header exists; no transaction could be durable.
    auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CLOEXEC));
    if (!fd.Valid() || io::Ftruncate(fd.Get(), 0) < 0) {
      return ErrnoStatus("clear incomplete WAL header");
    }
    return {};
  }

  std::ranges::sort(loaded, {}, [](const LoadedSegment &segment) { return segment.header.segment_id; });
  const auto database_uuid = loaded.front().header.database_uuid;
  for (std::size_t index = 0; index < loaded.size(); ++index) {
    const auto &segment = loaded[index];
    if (segment.header.database_uuid != database_uuid ||
        (index != 0 && segment.header.segment_id != loaded[index - 1].header.segment_id + 1U)) {
      return Status::Corruption("WAL segment identity sequence is missing or duplicated");
    }
    if (segment.file.active && index + 1 != loaded.size()) {
      return Status::Corruption("active WAL segment is not the newest segment");
    }
  }

  auto database_state = ReadDatabaseStateForRecovery(db_path);
  if (!database_state) {
    return database_state.error();
  }
  if (database_state->has_value() && database_state->value().database_uuid != database_uuid) {
    return Status::InvalidArgument("write-ahead log does not belong to this database: " + wal_path.string());
  }

  auto encoded_records = std::vector<char>{};
  auto expected_lsn = loaded.front().header.starting_lsn;
  auto expected_sequence = std::uint32_t{0};
  auto active_present = false;
  auto cleanup_needed = active_partial || loaded.size() > 1;
  for (std::size_t segment_index = 0; segment_index < loaded.size(); ++segment_index) {
    const auto &segment = loaded[segment_index];
    active_present = active_present || segment.file.active;
    if (segment.header.starting_lsn != expected_lsn) {
      return Status::Corruption("WAL segment starting LSN breaks the durable sequence");
    }
    auto offset = std::size_t{wal_format::HEADER_BYTES};
    auto last_type = std::optional<wal_format::RecordType>{};
    while (offset < segment.bytes.size()) {
      const auto remaining = std::as_bytes(std::span{segment.bytes}).subspan(offset);
      const auto total = storage::GetLittleEndian<std::uint32_t>(remaining, 0);
      if (!total || *total < wal_format::RECORD_HEADER_BYTES || *total > MAX_RECORD_BYTES ||
          *total > remaining.size()) {
        if (!segment.file.active || segment_index + 1 != loaded.size()) {
          return Status::Corruption("archived WAL segment has a torn record");
        }
        cleanup_needed = true;
        break;
      }
      const auto encoded_record = remaining.first(*total);
      const auto record = wal_format::DecodeRecord(encoded_record);
      if (!record) {
        const auto torn_checksum = record.error().Message() == "WAL record checksum mismatch" && segment.file.active &&
                                   segment_index + 1 == loaded.size() && offset + *total == segment.bytes.size();
        if (torn_checksum) {
          cleanup_needed = true;
          break;
        }
        return record.error();
      }
      if (record->lsn != expected_lsn || record->record_sequence != expected_sequence) {
        return Status::Corruption("WAL record sequence is missing, duplicated, or reordered");
      }
      encoded_records.insert(encoded_records.end(), segment.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             segment.bytes.begin() + static_cast<std::ptrdiff_t>(offset + *total));
      ++expected_lsn;
      ++expected_sequence;
      last_type = record->type;
      if (record->type == wal_format::RecordType::Commit) {
        expected_sequence = 0;
      }
      offset += *total;
    }
    if (segment.bytes.size() != wal_format::HEADER_BYTES) {
      cleanup_needed = true;
    }
    const auto has_following_segment = segment_index + 1 != loaded.size() || active_partial;
    if (has_following_segment && last_type != wal_format::RecordType::Commit) {
      return Status::Corruption("a WAL transaction crosses a segment boundary");
    }
  }
  cleanup_needed = cleanup_needed || !active_present;

  /*
  ** VALIDATE AND REDO COMPLETE TRANSACTIONS
  **
  ** Recovery is intentionally still one pass here. Transactions covered by
  ** the selected superblock are validated but not replayed; this is what makes
  ** crashes during archive cleanup idempotent. The recovery subsystem later
  ** separates complete validation from physical replay.
  */
  const auto checkpoint_lsn = database_state->has_value() ? database_state->value().checkpoint_lsn : 0;
  auto db_fd = UniqueFd{};
  auto created_db_file = false;
  auto run = std::vector<char>{};
  auto run_first_lsn = loaded.front().header.starting_lsn;
  auto durable_next_lsn = std::max(loaded.front().header.starting_lsn, checkpoint_lsn + 1U);
  auto last_transaction = std::optional<wal_format::DecodedTransaction>{};
  auto applied_transactions = std::uint64_t{0};
  auto last_transaction_id = std::optional<std::uint64_t>{};
  auto saw_uncheckpointed_transaction = false;
  auto offset = std::size_t{0};
  while (offset < encoded_records.size()) {
    const auto bytes = std::as_bytes(std::span{encoded_records}).subspan(offset);
    const auto total = storage::GetLittleEndian<std::uint32_t>(bytes, 0);
    TINYDB_CHECK(total && *total <= bytes.size(), "validated WAL record geometry changed during recovery");
    const auto record_bytes = bytes.first(*total);
    const auto record = wal_format::DecodeRecord(record_bytes);
    TINYDB_CHECK(record.has_value(), "validated WAL record failed a second decode");
    if (run.empty()) {
      run_first_lsn = record->lsn;
    }
    run.insert(run.end(), encoded_records.begin() + static_cast<std::ptrdiff_t>(offset),
               encoded_records.begin() + static_cast<std::ptrdiff_t>(offset + *total));
    offset += *total;
    if (record->type != wal_format::RecordType::Commit) {
      continue;
    }

    auto transaction = wal_format::DecodeTransaction(std::as_bytes(std::span{run}), run_first_lsn);
    if (!transaction) {
      return transaction.error();
    }
    if (last_transaction_id && transaction->transaction_id != *last_transaction_id + 1U) {
      return Status::Corruption("WAL transaction ID sequence is missing or duplicated");
    }
    last_transaction_id = transaction->transaction_id;
    durable_next_lsn = transaction->next_lsn;
    if (transaction->commit_lsn > checkpoint_lsn) {
      if (transaction->first_lsn <= checkpoint_lsn) {
        return Status::Corruption("database checkpoint cuts through a WAL transaction");
      }
      if (!saw_uncheckpointed_transaction) {
        if (transaction->first_lsn != checkpoint_lsn + 1U ||
            (database_state->has_value() &&
             transaction->transaction_id != database_state->value().transaction_id + 1U)) {
          return Status::Corruption("WAL sequence is missing the first transaction after the checkpoint");
        }
        saw_uncheckpointed_transaction = true;
      }
      if (!db_fd.Valid()) {
        db_fd = UniqueFd(io::Open(db_path, O_RDWR | O_CLOEXEC));
        if (!db_fd.Valid() && errno == ENOENT) {
          db_fd = UniqueFd(io::Open(db_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
          created_db_file = db_fd.Valid();
        }
        if (!db_fd.Valid()) {
          return ErrnoStatus("open database for recovery");
        }
      }
      if (io::Ftruncate(db_fd.Get(), transaction->state.high_water_page_id * PAGE_SIZE) < 0) {
        return ErrnoStatus("extend database during recovery");
      }
      for (const auto &page : transaction->pages) {
        if (auto status = FullPwrite(db_fd.Get(), page.bytes.data(), page.bytes.size(), page.page_id * PAGE_SIZE);
            !status.Ok()) {
          return status;
        }
      }
      last_transaction = std::move(*transaction);
      last_transaction->pages.clear();
      ++applied_transactions;
    }
    run.clear();
  }
  if (!run.empty() && !active_present && !active_partial) {
    return Status::Corruption("archived WAL ends with an incomplete transaction");
  }
  if (!saw_uncheckpointed_transaction && loaded.front().header.starting_lsn > checkpoint_lsn + 1U) {
    return Status::Corruption("WAL segment sequence begins after the checkpoint frontier");
  }

  if (last_transaction) {
    const auto base_generation = database_state->has_value() ? database_state->value().generation : 0;
    const auto superblock =
        EncodeRecoverySuperblock(*last_transaction, database_uuid, base_generation + applied_transactions);
    if (!superblock) {
      return superblock.error();
    }
    for (const auto page_id : {SUPERBLOCK_A_PAGE_ID, SUPERBLOCK_B_PAGE_ID}) {
      if (auto status = FullPwrite(db_fd.Get(), superblock->data(), superblock->size(), page_id * PAGE_SIZE);
          !status.Ok()) {
        return status;
      }
    }
    if (io::Fsync(db_fd.Get()) < 0) {
      return ErrnoStatus("fsync recovered database");
    }
    if (created_db_file) {
      if (auto status = SyncParentDirectory(db_path); !status.Ok()) {
        return status;
      }
    }
  }

  if (!cleanup_needed) {
    return {};
  }

  // Install a clean active header before deleting covered archives. A crash at
  // any cleanup boundary therefore leaves either the old complete sequence or
  // a clean active segment plus harmless checkpoint-covered archives.
  auto active_fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!active_fd.Valid() || io::Ftruncate(active_fd.Get(), 0) < 0) {
    return ErrnoStatus("replace active WAL segment");
  }
  const auto active_segment_id = loaded.back().header.segment_id + (active_present ? 0U : 1U);
  const auto clean_header = wal_format::EncodeHeader(wal_format::Header{
      .database_uuid = database_uuid,
      .segment_id = active_segment_id,
      .starting_lsn = durable_next_lsn,
  });
  if (!clean_header) {
    return clean_header.error();
  }
  if (auto status = FullPwrite(active_fd.Get(), clean_header->data(), clean_header->size(), 0); !status.Ok()) {
    return status;
  }
  if (io::Fsync(active_fd.Get()) < 0) {
    return ErrnoStatus("fsync clean WAL segment");
  }

  auto removed_archive = false;
  for (const auto &segment : loaded) {
    if (segment.file.active) {
      continue;
    }
    if (io::Unlink(segment.file.path) < 0 && errno != ENOENT) {
      return ErrnoStatus("remove checkpointed WAL segment");
    }
    removed_archive = true;
  }
  if (removed_archive || !active_present) {
    if (auto status = SyncParentDirectory(wal_path); !status.Ok()) {
      return status;
    }
  }
  return {};
}

auto Wal::Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
               std::uint64_t next_transaction_id, std::uint64_t starting_lsn,
               std::uint64_t max_segment_bytes) -> Result<Wal> {
  if (next_transaction_id == 0 || max_segment_bytes <= wal_format::HEADER_BYTES) {
    return std::unexpected(Status::InvalidArgument("invalid WAL transaction frontier"));
  }
  const auto requested_starting_lsn = starting_lsn;
  if (starting_lsn == 0) {
    starting_lsn = 1;
  }
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.Valid()) {
    return std::unexpected(ErrnoStatus("open"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(ErrnoStatus("fstat"));
  }
  Wal wal(std::move(fd), wal_path, database_uuid, 1, static_cast<std::uint64_t>(file_stat.st_size), next_transaction_id,
          starting_lsn, max_segment_bytes);

  if (file_stat.st_size == 0) {
    // Fresh WAL durability ordering is contents -> file fsync -> directory
    // fsync. A crash before completion leaves the short-header case Recover
    // handles without pretending any transaction committed.
    const auto encoded =
        wal_format::EncodeHeader(wal_format::Header{.database_uuid = database_uuid, .starting_lsn = starting_lsn});
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
  if (requested_starting_lsn != 0 && header->starting_lsn != requested_starting_lsn) {
    return std::unexpected(Status::Corruption("WAL starting LSN disagrees with the database checkpoint"));
  }
  wal.segment_id_ = header->segment_id;
  wal.next_lsn_ = header->starting_lsn;
  return wal;
}

auto Wal::RotateSegment() -> Status {
  TINYDB_CHECK(size_bytes_ > wal_format::HEADER_BYTES, "rotating an empty WAL segment");
  const auto archive_path = SegmentPathFor(wal_path_, segment_id_);
  auto exists_error = std::error_code{};
  if (std::filesystem::exists(archive_path, exists_error) || exists_error) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("next WAL archive path is not safely available");
  }
  if (io::Rename(wal_path_, archive_path) < 0) {
    return ErrnoStatus("archive WAL segment");
  }
  // A failed directory sync makes the active/archive name transition
  // indeterminate across power loss. The old fd remains valid, but appending
  // through it would no longer have one discoverable active identity.
  if (auto status = SyncParentDirectory(wal_path_); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("WAL segment archive synchronization failed");
  }

  auto next_fd = UniqueFd(io::Open(wal_path_, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
  if (!next_fd.Valid()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not create the next WAL segment");
  }
  const auto next_segment_id = segment_id_ + 1U;
  if (next_segment_id == 0) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("WAL segment ID space exhausted after archive");
  }
  const auto header = wal_format::EncodeHeader(wal_format::Header{
      .database_uuid = database_uuid_,
      .segment_id = next_segment_id,
      .starting_lsn = next_lsn_,
  });
  if (!header) {
    needs_recovery_ = true;
    return Status::NeedsRecovery(header.error().Message());
  }
  if (auto status = FullPwrite(next_fd.Get(), header->data(), header->size(), 0); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not write the next WAL segment header");
  }
  if (io::Fsync(next_fd.Get()) < 0) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("could not synchronize the next WAL segment header");
  }
  if (auto status = SyncParentDirectory(wal_path_); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("next WAL segment directory entry is indeterminate");
  }

  retained_size_bytes_ += size_bytes_;
  archived_segments_.push_back(archive_path);
  fd_ = std::move(next_fd);
  segment_id_ = next_segment_id;
  size_bytes_ = wal_format::HEADER_BYTES;
  return {};
}

void Wal::AppendPageImage(page_id_t page_id, const char *data) {
  TINYDB_CHECK(page_id >= FIRST_DATA_PAGE_ID, "logging a superblock as a WAL page image");
  auto image = PendingPageImage{.page_id = page_id, .bytes = {}};
  std::memcpy(image.bytes.data(), data, PAGE_SIZE);
  pending_.push_back(std::move(image));
}

void Wal::DiscardPending() {
  // Durable size and transaction ID do not advance. A retry/reopen therefore
  // cannot mistake abandoned memory records for a committed transaction.
  pending_.clear();
}

auto Wal::NextCommitLsn(std::size_t page_count) const -> Result<std::uint64_t> {
  if (page_count == 0 || page_count > std::numeric_limits<std::uint32_t>::max() - 2U ||
      next_lsn_ >= std::numeric_limits<std::uint64_t>::max() ||
      page_count > std::numeric_limits<std::uint64_t>::max() - next_lsn_ - 1U) {
    return std::unexpected(Status::ResourceExhausted("WAL LSN space exhausted"));
  }
  return next_lsn_ + page_count + 1U;
}

auto Wal::Commit(txn::DatabaseState state) -> Result<CommitInfo> {
  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  TINYDB_CHECK(!pending_.empty(), "committing an operation that logged no page images");
  if (needs_recovery_) {
    return std::unexpected(Status::NeedsRecovery("WAL tail is not trustworthy; reopen the database"));
  }
  // A zero transaction ID asks the WAL to assign its current frontier. A
  // nonzero ID was assigned by a higher-level coordinator and must agree
  // before the first byte is appended; discovering disagreement after fsync
  // would turn a programmer error into a durable, unpublished transaction.
  if (state.transaction_id != 0 && state.transaction_id != next_transaction_id_) {
    return std::unexpected(Status::InvalidArgument("database state has the wrong WAL transaction ID"));
  }

  auto views = std::vector<wal_format::PageImageView>{};
  views.reserve(pending_.size());
  for (const auto &page : pending_) {
    views.push_back(wal_format::PageImageView{.page_id = page.page_id, .bytes = page.bytes});
  }
  auto transaction = wal_format::EncodeTransaction(next_transaction_id_, next_lsn_, views, state);
  if (!transaction) {
    return std::unexpected(transaction.error());
  }

  // Rotation is soft: a transaction larger than the configured target owns an
  // oversized empty segment rather than being split across files.
  if (size_bytes_ > wal_format::HEADER_BYTES && size_bytes_ + transaction->bytes.size() > max_segment_bytes_) {
    if (auto status = RotateSegment(); !status.Ok()) {
      pending_.clear();
      return std::unexpected(std::move(status));
    }
  }

  /*
  ** DURABILITY POINT
  **
  ** One contiguous append preserves record order. The subsequent fsync is the
  ** exact point after which StorageEngine may acknowledge durability. Nothing
  ** below advances the known-good tail until that synchronization succeeds.
  */
  if (auto status = FullPwrite(fd_.Get(), transaction->bytes.data(), transaction->bytes.size(), size_bytes_);
      !status.Ok()) {
    /*
    ** A failed append cannot contain the complete final COMMIT record: the
    ** write loop stops at the first failed/short syscall and COMMIT is last.
    ** Truncating to known_good_size therefore converts the result into a
    ** definite abort. If that repair fails, live appends are forbidden because
    ** their offset could splice into an unknown tail.
    */
    if (io::Ftruncate(fd_.Get(), size_bytes_) < 0) {
      needs_recovery_ = true;
      pending_.clear();
      return std::unexpected(Status::NeedsRecovery("WAL append and known-good tail repair both failed"));
    }
    pending_.clear();
    return std::unexpected(std::move(status));
  }
  if (io::Fsync(fd_.Get()) < 0) {
    // The kernel may have persisted the COMMIT despite reporting failure. No
    // live repair can turn that into a definite outcome without another
    // ambiguous durability boundary, so recovery must decide on reopen.
    needs_recovery_ = true;
    pending_.clear();
    return std::unexpected(Status::IndeterminateCommit(ErrnoStatus("fsync").Message()));
  }
  size_bytes_ += transaction->bytes.size();
  next_lsn_ = transaction->next_lsn;
  pending_.clear();
  const auto result = CommitInfo{.transaction_id = next_transaction_id_, .commit_lsn = transaction->commit_lsn};
  ++next_transaction_id_;
  return result;
}

auto Wal::SizeBytes() const -> std::uint64_t { return retained_size_bytes_ + size_bytes_; }

auto Wal::Reset() -> Status {
  TINYDB_CHECK(fd_.Valid(), "resetting a moved-from log");
  TINYDB_CHECK(pending_.empty(), "resetting the log mid-operation");
  // Replace the header so its starting LSN continues after the checkpointed
  // transaction even though the physical byte offset returns to HEADER_BYTES.
  if (io::Ftruncate(fd_.Get(), 0) < 0) {
    return ErrnoStatus("ftruncate");
  }
  const auto header = wal_format::EncodeHeader(wal_format::Header{
      .database_uuid = database_uuid_,
      .segment_id = segment_id_,
      .starting_lsn = next_lsn_,
  });
  if (!header) {
    return header.error();
  }
  if (auto status = FullPwrite(fd_.Get(), header->data(), header->size(), 0); !status.Ok()) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("checkpoint reset could not restore the WAL header: " + status.Message());
  }
  if (io::Fsync(fd_.Get()) < 0) {
    needs_recovery_ = true;
    return Status::NeedsRecovery("checkpoint WAL reset synchronization failed");
  }
  auto removed_archive = false;
  for (const auto &archive : archived_segments_) {
    if (io::Unlink(archive) < 0 && errno != ENOENT) {
      needs_recovery_ = true;
      return Status::NeedsRecovery("checkpoint could not remove a covered WAL segment");
    }
    removed_archive = true;
  }
  if (removed_archive) {
    if (auto status = SyncParentDirectory(wal_path_); !status.Ok()) {
      needs_recovery_ = true;
      return Status::NeedsRecovery("checkpointed WAL segment deletion is not durable");
    }
  }
  size_bytes_ = wal_format::HEADER_BYTES;
  retained_size_bytes_ = 0;
  archived_segments_.clear();
  return {};
}

}  // namespace tinydb
