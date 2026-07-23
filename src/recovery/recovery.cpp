#include "recovery/recovery.h"

#include "io/file_io.h"
#include "io/syscalls.h"
#include "io/unique_fd.h"
#include "storage/encoding.h"
#include "storage/page.h"
#include "storage/superblock.h"
#include "wal/wal_codec.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace tinydb::recovery {
namespace {

constexpr auto MAX_RECORD_BYTES = wal_format::RECORD_HEADER_BYTES + wal_format::PAGE_IMAGE_PAYLOAD_BYTES;
constexpr auto COMMIT_RECORD_BYTES = wal_format::RECORD_HEADER_BYTES + wal_format::COMMIT_STATE_PAYLOAD_BYTES;
constexpr auto MAX_FILE_PAGES =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / static_cast<std::uint64_t>(PAGE_SIZE);

struct LoadedLog final {
  wal_format::Header header;
  std::vector<wal_format::DecodedTransaction> transactions;
  bool cleanup_needed{false};
};

struct RecoveryPlan final {
  DatabaseUuid database_uuid{};
  std::vector<wal_format::DecodedTransaction> transactions;
  std::optional<storage::SuperblockPage> recovered_superblock;
  page_id_t superblock_page_id{SUPERBLOCK_A_PAGE_ID};
  std::uint64_t durable_lsn{0};
  bool cleanup_needed{false};
};

auto ReadFile(int fd, std::size_t size) -> Result<std::vector<char>> {
  auto bytes = std::vector<char>(size);
  const auto read = io::FullPread(fd, bytes.data(), bytes.size(), 0);
  if (!read) {
    return std::unexpected(read.error());
  }
  if (*read != bytes.size()) {
    return std::unexpected(Status::Corruption("WAL changed while held under exclusive ownership"));
  }
  return bytes;
}

auto ClearPartialWal(const std::filesystem::path &wal_path) -> Status {
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CLOEXEC));
  if (!fd.Valid() || io::Ftruncate(fd.Get(), 0) < 0) {
    return io::ErrnoStatus("clear incomplete WAL header");
  }
  if (io::Fsync(fd.Get()) < 0) {
    return io::ErrnoStatus("fsync cleared WAL");
  }
  return {};
}

auto HasValidRecordAfter(std::span<const std::byte> bytes, std::size_t damaged_offset) -> bool {
  // Both record sizes and the file header are eight-byte aligned. Search only
  // this exceptional damage path: finding a later checksummed record proves
  // that malformed framing is in the durable middle rather than an incomplete
  // append tail.
  for (auto offset = damaged_offset + 8U; offset + wal_format::RECORD_HEADER_BYTES <= bytes.size(); offset += 8U) {
    const auto remaining = bytes.subspan(offset);
    const auto total = storage::GetLittleEndian<std::uint32_t>(remaining, wal_format::record_offset::TOTAL_BYTES);
    if (!total || (*total != MAX_RECORD_BYTES && *total != COMMIT_RECORD_BYTES) || *total > remaining.size()) {
      continue;
    }
    if (wal_format::DecodeRecord(remaining.first(*total))) {
      return true;
    }
  }
  return false;
}

auto LoadLog(const std::filesystem::path &wal_path) -> Result<std::optional<LoadedLog>> {
  auto fd = UniqueFd(io::Open(wal_path, O_RDONLY | O_CLOEXEC));
  if (!fd.Valid()) {
    if (errno == ENOENT) {
      return std::optional<LoadedLog>{};
    }
    return std::unexpected(io::ErrnoStatus("open WAL for recovery"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(io::ErrnoStatus("fstat WAL"));
  }
  if (file_stat.st_size < 0) {
    return std::unexpected(Status::Corruption("WAL has a negative size"));
  }
  if (file_stat.st_size < static_cast<off_t>(wal_format::HEADER_BYTES)) {
    // Reset truncates only after a covering database superblock is durable.
    // A crash while replacing the clean header therefore leaves no live redo.
    fd = UniqueFd{};
    if (auto status = ClearPartialWal(wal_path); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    return std::optional<LoadedLog>{};
  }

  const auto size = static_cast<std::size_t>(file_stat.st_size);
  auto bytes_result = ReadFile(fd.Get(), size);
  if (!bytes_result) {
    return std::unexpected(bytes_result.error());
  }
  auto bytes = std::move(*bytes_result);
  auto all = std::as_bytes(std::span{bytes});
  const auto header = wal_format::DecodeHeader(all.first(wal_format::HEADER_BYTES));
  if (!header) {
    return std::unexpected(header.error());
  }

  auto log = LoadedLog{.header = *header, .transactions = {}, .cleanup_needed = false};
  auto offset = std::size_t{wal_format::HEADER_BYTES};
  auto run_offset = offset;
  auto current_lsn = std::uint64_t{0};
  auto expected_lsn = header->starting_lsn;
  while (offset < all.size()) {
    const auto remaining = all.subspan(offset);
    const auto total = storage::GetLittleEndian<std::uint32_t>(remaining, wal_format::record_offset::TOTAL_BYTES);
    if (!total || *total < wal_format::RECORD_HEADER_BYTES || *total > MAX_RECORD_BYTES || *total > remaining.size()) {
      if (HasValidRecordAfter(all, offset)) {
        return std::unexpected(Status::Corruption("invalid WAL framing before a complete later record"));
      }
      log.cleanup_needed = true;
      break;
    }
    const auto encoded_record = remaining.first(*total);
    const auto record = wal_format::DecodeRecord(encoded_record);
    if (!record) {
      const auto torn_final_record = record.error().Message() == "WAL record checksum mismatch" &&
                                     offset + *total == all.size() && !HasValidRecordAfter(all, offset);
      if (torn_final_record) {
        log.cleanup_needed = true;
        break;
      }
      return std::unexpected(record.error());
    }
    if (current_lsn == 0) {
      run_offset = offset;
      current_lsn = record->lsn;
      if (current_lsn != expected_lsn || record->type != wal_format::RecordType::PageImage) {
        return std::unexpected(Status::Corruption("WAL transaction sequence is missing or reordered"));
      }
    } else if (record->lsn != current_lsn) {
      return std::unexpected(Status::Corruption("WAL transaction records disagree on commit LSN"));
    }

    offset += *total;
    if (record->type == wal_format::RecordType::CommitState) {
      const auto run = all.subspan(run_offset, offset - run_offset);
      auto transaction = wal_format::DecodeTransaction(run, current_lsn);
      if (!transaction) {
        return std::unexpected(transaction.error());
      }
      log.transactions.push_back(std::move(*transaction));
      if (current_lsn == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(Status::Corruption("WAL LSN sequence overflows"));
      }
      expected_lsn = current_lsn + 1U;
      current_lsn = 0;
    }
  }
  if (current_lsn != 0) {
    log.cleanup_needed = true;
  }
  log.cleanup_needed = log.cleanup_needed || all.size() != wal_format::HEADER_BYTES;
  return std::optional{std::move(log)};
}

auto ReadDatabaseBase(const std::filesystem::path &db_path) -> Result<std::optional<storage::SelectedSuperblock>> {
  auto fd = UniqueFd(io::Open(db_path, O_RDONLY | O_CLOEXEC));
  if (!fd.Valid()) {
    if (errno == ENOENT) {
      return std::optional<storage::SelectedSuperblock>{};
    }
    return std::unexpected(io::ErrnoStatus("open database"));
  }
  struct stat file_stat {};
  if (io::Fstat(fd.Get(), &file_stat) < 0) {
    return std::unexpected(io::ErrnoStatus("fstat database"));
  }
  if (file_stat.st_size == 0) {
    return std::optional<storage::SelectedSuperblock>{};
  }
  if (file_stat.st_size < static_cast<off_t>(FIRST_DATA_PAGE_ID * PAGE_SIZE)) {
    return std::unexpected(Status::UnsupportedFormat("database does not contain TinyDB dual superblocks"));
  }
  if (file_stat.st_size % static_cast<off_t>(PAGE_SIZE) != 0) {
    return std::unexpected(Status::Corruption("database file ends in a partial page"));
  }

  auto prefix = std::array<char, FIRST_DATA_PAGE_ID * PAGE_SIZE>{};
  const auto read = io::FullPread(fd.Get(), prefix.data(), prefix.size(), 0);
  if (!read) {
    return std::unexpected(read.error());
  }
  if (*read != prefix.size()) {
    return std::unexpected(Status::Corruption("database superblocks are truncated"));
  }
  if (std::ranges::all_of(prefix, [](char byte) { return byte == 0; })) {
    return std::optional<storage::SelectedSuperblock>{};
  }

  const auto bytes = std::as_bytes(std::span{prefix});
  auto selected = storage::SelectSuperblock(bytes.first(PAGE_SIZE), bytes.subspan(PAGE_SIZE, PAGE_SIZE));
  if (!selected) {
    return std::unexpected(selected.error());
  }
  if (selected->value.high_water_page_id >
      static_cast<std::uint64_t>(file_stat.st_size) / static_cast<std::uint64_t>(PAGE_SIZE)) {
    return std::unexpected(Status::Corruption("superblock allocation frontier exceeds the database file"));
  }
  if (selected->value.high_water_page_id > MAX_FILE_PAGES) {
    return std::unexpected(Status::Corruption("superblock allocation frontier exceeds the platform file limit"));
  }
  return std::optional{*selected};
}

auto EncodeRecoveredSuperblock(const wal_format::DecodedTransaction &transaction,
                               const storage::SelectedSuperblock &base) -> Result<storage::SuperblockPage> {
  if (base.value.generation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("superblock generation space exhausted"));
  }
  return storage::EncodeSuperblock(storage::Superblock{
      .database_uuid = base.value.database_uuid,
      .generation = base.value.generation + 1U,
      .checkpoint_lsn = transaction.commit_lsn,
      .root_page_id = transaction.state.root_page_id,
      .allocator_root_page_id = transaction.state.allocator_root_page_id,
      .high_water_page_id = transaction.state.high_water_page_id,
      .required_features = base.value.required_features,
      .optional_features = base.value.optional_features,
  });
}

auto BuildPlan(LoadedLog log, const storage::SelectedSuperblock &base) -> Result<RecoveryPlan> {
  if (base.value.database_uuid != log.header.database_uuid) {
    return std::unexpected(Status::InvalidArgument("write-ahead log does not belong to this database"));
  }
  if (base.value.checkpoint_lsn == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("checkpoint LSN space exhausted"));
  }
  if (log.header.starting_lsn > base.value.checkpoint_lsn + 1U) {
    return std::unexpected(Status::Corruption("WAL begins after the database checkpoint frontier"));
  }

  auto plan = RecoveryPlan{
      .database_uuid = log.header.database_uuid,
      .transactions = {},
      .recovered_superblock = std::nullopt,
      .superblock_page_id = SUPERBLOCK_A_PAGE_ID,
      .durable_lsn = base.value.checkpoint_lsn,
      .cleanup_needed = log.cleanup_needed,
  };
  auto previous_high_water = base.value.high_water_page_id;
  auto saw_checkpoint_transaction = log.header.starting_lsn == base.value.checkpoint_lsn + 1U;
  auto expected_lsn = log.header.starting_lsn;
  for (auto &transaction : log.transactions) {
    if (transaction.commit_lsn != expected_lsn) {
      return std::unexpected(Status::Corruption("WAL transaction LSN sequence is missing or reordered"));
    }
    if (expected_lsn == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(Status::Corruption("WAL transaction LSN sequence overflows"));
    }
    ++expected_lsn;

    if (transaction.commit_lsn <= base.value.checkpoint_lsn) {
      if (transaction.commit_lsn == base.value.checkpoint_lsn) {
        saw_checkpoint_transaction = true;
        if (transaction.state.root_page_id != base.value.root_page_id ||
            transaction.state.allocator_root_page_id != base.value.allocator_root_page_id ||
            transaction.state.high_water_page_id != base.value.high_water_page_id) {
          return std::unexpected(Status::Corruption("covered WAL state disagrees with the database checkpoint"));
        }
      }
      continue;
    }
    if (transaction.commit_lsn != plan.durable_lsn + 1U || transaction.state.high_water_page_id < previous_high_water ||
        transaction.state.high_water_page_id > MAX_FILE_PAGES) {
      return std::unexpected(Status::Corruption("WAL live transaction frontier is inconsistent with its base"));
    }
    for (const auto &page : transaction.pages) {
      if (page.page_id >= transaction.state.high_water_page_id) {
        return std::unexpected(Status::Corruption("WAL page image lies beyond its allocation frontier"));
      }
    }
    transaction.state.checkpoint_lsn = base.value.checkpoint_lsn;
    previous_high_water = transaction.state.high_water_page_id;
    plan.durable_lsn = transaction.commit_lsn;
    plan.transactions.push_back(std::move(transaction));
  }
  if (!saw_checkpoint_transaction) {
    return std::unexpected(Status::Corruption("WAL does not contain its claimed checkpoint prefix"));
  }

  if (!plan.transactions.empty()) {
    auto encoded = EncodeRecoveredSuperblock(plan.transactions.back(), base);
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    plan.recovered_superblock = *encoded;
    plan.superblock_page_id = base.slot == storage::SuperblockSlot::A ? SUPERBLOCK_B_PAGE_ID : SUPERBLOCK_A_PAGE_ID;
  }
  return plan;
}

auto Redo(const std::filesystem::path &db_path, const RecoveryPlan &plan) -> Status {
  if (plan.transactions.empty()) {
    return {};
  }
  if (!plan.recovered_superblock) {
    return Status::Corruption("recovery plan is missing its final superblock");
  }
  const auto &recovered_superblock = *plan.recovered_superblock;
  auto fd = UniqueFd(io::Open(db_path, O_RDWR | O_CLOEXEC));
  if (!fd.Valid()) {
    return io::ErrnoStatus("open database for recovery");
  }
  const auto &final_state = plan.transactions.back().state;
  if (io::Ftruncate(fd.Get(), final_state.high_water_page_id * PAGE_SIZE) < 0) {
    return io::ErrnoStatus("extend database during recovery");
  }
  for (const auto &transaction : plan.transactions) {
    for (const auto &page : transaction.pages) {
      if (auto status = io::FullPwrite(fd.Get(), page.bytes.data(), page.bytes.size(), page.page_id * PAGE_SIZE);
          !status.Ok()) {
        return status;
      }
    }
  }
  if (io::Fsync(fd.Get()) < 0) {
    return io::ErrnoStatus("fsync recovered database pages");
  }
  if (auto status = io::FullPwrite(fd.Get(), recovered_superblock.data(), recovered_superblock.size(),
                                   plan.superblock_page_id * PAGE_SIZE);
      !status.Ok()) {
    return status;
  }
  if (io::Fsync(fd.Get()) < 0) {
    return io::ErrnoStatus("fsync recovered superblock");
  }
  return {};
}

auto ResetWal(const std::filesystem::path &wal_path, const RecoveryPlan &plan) -> Status {
  if (!plan.cleanup_needed) {
    return {};
  }
  if (plan.durable_lsn == std::numeric_limits<std::uint64_t>::max()) {
    return Status::ResourceExhausted("WAL LSN space exhausted during recovery");
  }
  auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CLOEXEC));
  if (!fd.Valid() || io::Ftruncate(fd.Get(), 0) < 0) {
    return io::ErrnoStatus("reset recovered WAL");
  }
  const auto header = wal_format::EncodeHeader(wal_format::Header{
      .database_uuid = plan.database_uuid,
      .starting_lsn = plan.durable_lsn + 1U,
  });
  if (!header) {
    return header.error();
  }
  if (auto status = io::FullPwrite(fd.Get(), header->data(), header->size(), 0); !status.Ok()) {
    return status;
  }
  if (io::Fsync(fd.Get()) < 0) {
    return io::ErrnoStatus("fsync recovered WAL");
  }
  return {};
}

}  // namespace

auto Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status {
  auto loaded_result = LoadLog(wal_path);
  if (!loaded_result) {
    return loaded_result.error();
  }
  auto loaded = std::move(*loaded_result);
  if (!loaded) {
    return {};
  }
  auto base_result = ReadDatabaseBase(db_path);
  if (!base_result) {
    return base_result.error();
  }
  auto base = *base_result;
  if (!base) {
    return Status::Corruption("database base is missing; WAL is not a complete backup");
  }
  auto plan = BuildPlan(std::move(*loaded), *base);
  if (!plan) {
    return plan.error();
  }
  if (auto status = Redo(db_path, *plan); !status.Ok()) {
    return status;
  }
  return ResetWal(wal_path, *plan);
}

}  // namespace tinydb::recovery
