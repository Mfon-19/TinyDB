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

  const auto database_state = ReadDatabaseStateForRecovery(db_path);
  if (!database_state) {
    return database_state.error();
  }
  if (database_state->has_value() && database_state->value().database_uuid != header->database_uuid) {
    // Refuse before opening the database writable or applying a single page.
    return Status::InvalidArgument("write-ahead log does not belong to this database: " + wal_path.string());
  }

  // run_bytes retains exactly one transaction. Nothing is written until its
  // final COMMIT validates record order, page count, state, and both digests.
  auto db_fd = UniqueFd{};
  auto run_bytes = std::vector<char>{};
  auto run_first_lsn = header->starting_lsn;
  auto run_sequence = std::uint32_t{0};
  auto expected_lsn = header->starting_lsn;
  auto durable_next_lsn = header->starting_lsn;
  auto last_transaction = std::optional<wal_format::DecodedTransaction>{};
  auto committed_transactions = std::uint64_t{0};
  auto applied = false;
  auto created_db_file = false;
  auto offset = static_cast<std::uint64_t>(wal_format::HEADER_BYTES);

  /*
  ** PASS THROUGH THE LOG
  **
  ** The current implementation validates and replays each committed run in a
  ** single pass. Milestone 7 will separate validation from replay, but the
  ** committed-run and torn-tail rules here remain the protocol foundation.
  */
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
    if (record->lsn != expected_lsn || record->record_sequence != run_sequence) {
      return Status::Corruption("WAL record sequence is missing, duplicated, or reordered");
    }
    if (run_bytes.empty()) {
      run_first_lsn = record->lsn;
    }
    run_bytes.insert(run_bytes.end(), encoded_record.begin(), encoded_record.end());
    ++expected_lsn;
    ++run_sequence;

    if (record->type == wal_format::RecordType::Commit) {
      const auto transaction = wal_format::DecodeTransaction(std::as_bytes(std::span{run_bytes}), run_first_lsn);
      if (!transaction) {
        return transaction.error();
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
      if (io::Ftruncate(db_fd.Get(), transaction->state.high_water_page_id * PAGE_SIZE) < 0) {
        return ErrnoStatus("ftruncate");
      }
      for (const auto &page : transaction->pages) {
        // Full physical redo may extend the file. Reapplying after a recovery
        // crash simply overwrites the same offsets with the same bytes.
        if (auto status = FullPwrite(db_fd.Get(), page.bytes.data(), page.bytes.size(), page.page_id * PAGE_SIZE);
            !status.Ok()) {
          return status;
        }
      }
      durable_next_lsn = transaction->next_lsn;
      last_transaction = std::move(*transaction);
      last_transaction->pages.clear();
      ++committed_transactions;
      run_bytes.clear();
      run_sequence = 0;
      applied = true;
    }
    offset += encoded_record.size();
  }

  if (applied) {
    TINYDB_CHECK(last_transaction.has_value(), "recovery applied pages without committed database state");
    const auto base_generation = database_state->has_value() ? database_state->value().generation : 0;
    const auto superblock =
        EncodeRecoverySuperblock(*last_transaction, header->database_uuid, base_generation + committed_transactions);
    if (!superblock) {
      return superblock.error();
    }
    if (auto status = FullPwrite(db_fd.Get(), superblock->data(), superblock->size(), SUPERBLOCK_A_PAGE_ID * PAGE_SIZE);
        !status.Ok()) {
      return status;
    }
    if (auto status = FullPwrite(db_fd.Get(), superblock->data(), superblock->size(), SUPERBLOCK_B_PAGE_ID * PAGE_SIZE);
        !status.Ok()) {
      return status;
    }
    if (io::Fsync(db_fd.Get()) < 0) {
      // The WAL remains untouched on failure, so reopening retries all commits.
      return ErrnoStatus("fsync");
    }
  }
  if (applied && created_db_file) {
    // Only recovery that created the path must make its directory entry durable.
    if (auto status = SyncParentDirectory(db_path); !status.Ok()) {
      return status;
    }
  }
  if (wal_size > wal_format::HEADER_BYTES) {
    // Replacement is last: committed redo may be forgotten only after the
    // database and any newly created directory entry are durable. The new
    // header carries the first LSN not covered by the recovered checkpoint.
    if (io::Ftruncate(wal_fd.Get(), 0) < 0) {
      return ErrnoStatus("ftruncate");
    }
    const auto clean_header = wal_format::EncodeHeader(wal_format::Header{
        .database_uuid = header->database_uuid,
        .segment_id = header->segment_id,
        .starting_lsn = durable_next_lsn,
    });
    if (!clean_header) {
      return clean_header.error();
    }
    if (auto status = FullPwrite(wal_fd.Get(), clean_header->data(), clean_header->size(), 0); !status.Ok()) {
      return status;
    }
    if (io::Fsync(wal_fd.Get()) < 0) {
      return ErrnoStatus("fsync");
    }
  }
  return {};
}

auto Wal::Open(const std::filesystem::path &wal_path, const DatabaseUuid &database_uuid,
               std::uint64_t next_transaction_id, std::uint64_t starting_lsn) -> Result<Wal> {
  if (next_transaction_id == 0) {
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
  Wal wal(std::move(fd), database_uuid, static_cast<std::uint64_t>(file_stat.st_size), next_transaction_id,
          starting_lsn);

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
  wal.next_lsn_ = header->starting_lsn;
  return wal;
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

auto Wal::Commit(txn::DatabaseState state) -> Result<CommitInfo> {
  TINYDB_CHECK(fd_.Valid(), "committing on a moved-from log");
  TINYDB_CHECK(!pending_.empty(), "committing an operation that logged no page images");
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

  /*
  ** DURABILITY POINT
  **
  ** One contiguous append preserves record order. The subsequent fsync is the
  ** exact point after which StorageEngine may acknowledge durability. Nothing
  ** below advances the known-good tail until that synchronization succeeds.
  */
  if (auto status = FullPwrite(fd_.Get(), transaction->bytes.data(), transaction->bytes.size(), size_bytes_);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (io::Fsync(fd_.Get()) < 0) {
    // Do not advance size_bytes_ or clear pending_: the outcome is not treated
    // as acknowledged, and the owning engine poisons itself before more work.
    return std::unexpected(ErrnoStatus("fsync"));
  }
  size_bytes_ += transaction->bytes.size();
  next_lsn_ = transaction->next_lsn;
  pending_.clear();
  const auto result = CommitInfo{.transaction_id = next_transaction_id_, .commit_lsn = transaction->commit_lsn};
  ++next_transaction_id_;
  return result;
}

auto Wal::SizeBytes() const -> std::uint64_t { return size_bytes_; }

auto Wal::Reset() -> Status {
  TINYDB_CHECK(fd_.Valid(), "resetting a moved-from log");
  TINYDB_CHECK(pending_.empty(), "resetting the log mid-operation");
  // Replace the header so its starting LSN continues after the checkpointed
  // transaction even though the physical byte offset returns to HEADER_BYTES.
  if (io::Ftruncate(fd_.Get(), 0) < 0) {
    return ErrnoStatus("ftruncate");
  }
  const auto header =
      wal_format::EncodeHeader(wal_format::Header{.database_uuid = database_uuid_, .starting_lsn = next_lsn_});
  if (!header) {
    return header.error();
  }
  if (auto status = FullPwrite(fd_.Get(), header->data(), header->size(), 0); !status.Ok()) {
    return status;
  }
  if (io::Fsync(fd_.Get()) < 0) {
    return ErrnoStatus("fsync");
  }
  size_bytes_ = wal_format::HEADER_BYTES;
  return {};
}

}  // namespace tinydb
