#include "recovery/recovery.h"

#include "io/file_io.h"
#include "io/page_file.h"
#include "io/testable_posix.h"
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
  storage::SuperblockPage recovered_superblock{};
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

  auto log = LoadedLog{.header = *header, .transactions = {}, .cleanup_needed = all.size() != wal_format::HEADER_BYTES};
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
      break;
    }
    const auto encoded_record = remaining.first(*total);
    const auto record = wal_format::DecodeRecord(encoded_record);
    if (!record) {
      const auto torn_final_record = record.error().Message() == "WAL record checksum mismatch" &&
                                     offset + *total == all.size() && !HasValidRecordAfter(all, offset);
      if (torn_final_record) {
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
  return std::optional{std::move(log)};
}

auto ReadDatabaseBase(io::PageFile &database) -> Result<std::optional<storage::SelectedSuperblock>> {
  struct stat file_stat {};
  if (auto status = database.Stat(&file_stat); !status.Ok()) {
    return std::unexpected(std::move(status));
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

  auto page_a = storage::SuperblockPage{};
  auto page_b = storage::SuperblockPage{};
  if (auto status = database.ReadPage(SUPERBLOCK_A_PAGE_ID, page_a); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = database.ReadPage(SUPERBLOCK_B_PAGE_ID, page_b); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  const auto all_zero = [](const storage::SuperblockPage &page) {
    return std::ranges::all_of(page, [](std::byte byte) { return byte == std::byte{0}; });
  };
  if (all_zero(page_a) && all_zero(page_b)) {
    return std::optional<storage::SelectedSuperblock>{};
  }

  auto selected = storage::SelectSuperblock(page_a, page_b);
  if (!selected) {
    return std::unexpected(selected.error());
  }
  if (selected->value.logical_page_count >
      static_cast<std::uint64_t>(file_stat.st_size) / static_cast<std::uint64_t>(PAGE_SIZE)) {
    return std::unexpected(Status::Corruption("superblock logical page count exceeds the database file"));
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
      .logical_page_count = transaction.state.logical_page_count,
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
    return std::unexpected(Status::Corruption("WAL starting LSN leaves a gap after the database checkpoint"));
  }

  auto plan = RecoveryPlan{
      .database_uuid = log.header.database_uuid,
      .transactions = {},
      .recovered_superblock = {},
      .superblock_page_id = SUPERBLOCK_A_PAGE_ID,
      .durable_lsn = base.value.checkpoint_lsn,
      .cleanup_needed = log.cleanup_needed,
  };
  // durable_lsn starts at the database frontier. It advances only across
  // contiguous, complete WAL transactions that follow that frontier.
  auto previous_logical_page_count = base.value.logical_page_count;
  auto saw_checkpoint_transaction = log.header.starting_lsn == base.value.checkpoint_lsn + 1U;
  for (auto &transaction : log.transactions) {
    if (transaction.commit_lsn <= base.value.checkpoint_lsn) {
      if (transaction.commit_lsn == base.value.checkpoint_lsn) {
        saw_checkpoint_transaction = true;
        if (transaction.state.root_page_id != base.value.root_page_id ||
            transaction.state.allocator_root_page_id != base.value.allocator_root_page_id ||
            transaction.state.logical_page_count != base.value.logical_page_count) {
          return std::unexpected(Status::Corruption("covered WAL state disagrees with the database checkpoint"));
        }
      }
      continue;
    }
    if (transaction.commit_lsn != plan.durable_lsn + 1U) {
      return std::unexpected(Status::Corruption("WAL live transaction LSN does not follow its durable base"));
    }
    if (transaction.state.logical_page_count < previous_logical_page_count) {
      return std::unexpected(Status::Corruption("WAL logical page count decreases between transactions"));
    }
    if (transaction.state.logical_page_count > MAX_FILE_PAGES) {
      return std::unexpected(Status::Corruption("WAL logical page count exceeds the platform file limit"));
    }
    for (const auto &page : transaction.pages) {
      if (page.page_id >= transaction.state.logical_page_count) {
        return std::unexpected(Status::Corruption("WAL page image lies outside its logical page range"));
      }
    }
    transaction.state.checkpoint_lsn = base.value.checkpoint_lsn;
    previous_logical_page_count = transaction.state.logical_page_count;
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

/*
** RECOVERY PAGE I/O
**
** Recovery runs before construction of the cache and native I/O engine.
** PageFile therefore uses buffered I/O or synchronous direct I/O for redo.
**
** The decoded transactions own all source pages until Redo returns. Each batch
** contains pointers into this stable ownership. If direct I/O needs alignment
** staging, PageFile supplies a bounded buffer.
*/
auto Redo(io::PageFile &database, const RecoveryPlan &plan) -> Status {
  if (plan.transactions.empty()) {
    return {};
  }
  const auto &final_state = plan.transactions.back().state;
  if (auto status = database.EnsurePageCount(final_state.logical_page_count); !status.Ok()) {
    return status;
  }

  // Batches keep authenticated WAL order. Only adjacent physical IDs share a write.
  // A gap, repeated page, or full bounded run flushes before the next image.
  // Thus, batching does not reorder repeated updates across transactions.
  auto batch = std::array<const std::byte *, io::MAX_PAGE_WRITE_BATCH_PAGES>{};
  auto batch_first = page_id_t{0};
  auto batch_size = std::size_t{0};
  const auto flush_batch = [&]() -> Status {
    if (batch_size == 0) {
      return {};
    }
    const auto status = database.WritePages(batch_first, std::span<const std::byte *const>{batch.data(), batch_size});
    batch_size = 0;
    return status;
  };

  for (const auto &transaction : plan.transactions) {
    for (const auto &page : transaction.pages) {
      const auto continues = batch_size != 0 && page.page_id == batch_first + batch_size;
      if (batch_size == batch.size() || (batch_size != 0 && !continues)) {
        if (auto status = flush_batch(); !status.Ok()) {
          return status;
        }
      }
      if (batch_size == 0) {
        batch_first = page.page_id;
      }
      batch[batch_size++] = std::as_bytes(std::span{page.bytes}).data();
    }
  }
  if (auto status = flush_batch(); !status.Ok()) {
    return status;
  }
  // The first sync makes every recovered page image durable. The second sync
  // publishes the recovered superblock and its new durable LSN frontier.
  if (auto status = database.Sync(); !status.Ok()) {
    return status;
  }
  if (auto status = database.WritePage(plan.superblock_page_id, plan.recovered_superblock); !status.Ok()) {
    return status;
  }
  return database.Sync();
}

auto ResetWal(const std::filesystem::path &wal_path, const RecoveryPlan &plan) -> Status {
  if (!plan.cleanup_needed) {
    return {};
  }
  if (plan.durable_lsn == std::numeric_limits<std::uint64_t>::max()) {
    return Status::ResourceExhausted("WAL LSN space exhausted during recovery");
  }
  // Redo synchronized the recovered superblock before this reset. The new WAL
  // header therefore starts after the complete database frontier.
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

auto Recover(io::PageFile &database, const std::filesystem::path &wal_path) -> Status {
  auto loaded_result = LoadLog(wal_path);
  if (!loaded_result) {
    return loaded_result.error();
  }
  auto loaded = std::move(*loaded_result);
  if (!loaded) {
    return {};
  }
  auto base_result = ReadDatabaseBase(database);
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
  if (auto status = Redo(database, *plan); !status.Ok()) {
    return status;
  }
  return ResetWal(wal_path, *plan);
}

}  // namespace tinydb::recovery
