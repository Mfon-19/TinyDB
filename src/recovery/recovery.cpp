#include "recovery/recovery.h"

#include "io/unique_fd.h"
#include "storage/database_uuid.h"
#include "storage/page.h"
#include "util/check.h"

#include "io/file_io.h"
#include "io/syscalls.h"
#include "storage/encoding.h"
#include "storage/superblock.h"
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
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tinydb::recovery {
namespace {

/*
** TWO-PASS PHYSICAL RECOVERY
**
** The selected superblock names the checkpointed base.  WAL segments contain
** the complete physical suffix after that base.  Recovery never invokes a
** B+ tree operation and never infers logical mutations from page contents.
**
**   Pass 1: read -> frame -> authenticate -> validate continuity -> plan
**   Pass 2: extend -> redo pages -> fsync -> write inactive superblock
**           -> fsync -> replace/retire covered WAL segments
**
** Pass 1 opens WAL and database files read-only and performs every persistent
** input check.  Consequently a corruption result leaves the database file
** byte-for-byte unchanged.  During Pass 2 the old superblock remains the
** authority until all data pages are durable and the alternate superblock is
** written.  A crash before that point repeats redo; a crash afterward sees
** the recovered generation and treats leftover WAL as covered history.
*/

constexpr std::size_t MAX_RECORD_BYTES = wal_format::RECORD_HEADER_BYTES + wal_format::PAGE_IMAGE_PAYLOAD_BYTES;
constexpr page_id_t MAX_FILE_PAGES =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / static_cast<std::uint64_t>(PAGE_SIZE);

struct SegmentFile {
  std::filesystem::path path;
  std::uint64_t segment_id{0};
  bool active{false};
};

struct LoadedSegment {
  SegmentFile file;
  wal_format::Header header;
  std::vector<char> bytes;
};

struct LoadedLog {
  DatabaseUuid database_uuid{};
  std::vector<LoadedSegment> segments;
  std::vector<wal_format::DecodedTransaction> transactions;
  bool active_present{false};
  bool active_partial{false};
  bool cleanup_needed{false};
};

struct RecoveryPlan {
  DatabaseUuid database_uuid{};
  std::vector<wal_format::DecodedTransaction> transactions;
  std::optional<storage::SuperblockPage> recovered_superblock;
  page_id_t superblock_page_id{SUPERBLOCK_A_PAGE_ID};
  std::vector<std::filesystem::path> archive_paths;
  std::uint64_t active_segment_id{1};
  std::uint64_t durable_next_lsn{1};
  bool active_present{false};
  bool cleanup_needed{false};
};

/*
** Return every syntactically named archive and the active WAL.  Filenames are
** merely discovery hints; LoadLog later compares every archive name with its
** authenticated header before trusting its segment identity.
*/
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
    files.push_back(SegmentFile{.path = entry.path(), .segment_id = segment_id});
  }
  return files;
}

/*
** Load and validate segment framing without opening the database for writes.
** Only the newest active segment may end in a partial header, record, or
** transaction.  An archive is immutable, so the same condition there is
** durable-middle corruption rather than a discardable tail.
*/
auto LoadLog(const std::filesystem::path &wal_path) -> Result<std::optional<LoadedLog>> {
  auto discovered = DiscoverSegmentFiles(wal_path);
  if (!discovered) {
    return std::unexpected(discovered.error());
  }
  if (discovered->empty()) {
    return std::optional<LoadedLog>{};
  }

  auto log = LoadedLog{};
  for (auto file : *discovered) {
    auto fd = UniqueFd(io::Open(file.path, O_RDONLY | O_CLOEXEC));
    if (!fd.Valid()) {
      return std::unexpected(io::ErrnoStatus("open WAL segment"));
    }
    struct stat file_stat {};
    if (io::Fstat(fd.Get(), &file_stat) < 0) {
      return std::unexpected(io::ErrnoStatus("fstat WAL segment"));
    }
    if (file_stat.st_size < 0) {
      return std::unexpected(Status::Corruption("WAL segment has a negative size"));
    }
    const auto size = static_cast<std::uint64_t>(file_stat.st_size);
    if (size > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(Status::ResourceExhausted("WAL segment is too large to validate"));
    }
    auto bytes = std::vector<char>(size);
    const auto read = io::FullPread(fd.Get(), bytes.data(), bytes.size(), 0);
    if (!read) {
      return std::unexpected(read.error());
    }
    if (*read != bytes.size()) {
      return std::unexpected(Status::IoError("short read while loading WAL segment"));
    }

    if (size < wal_format::HEADER_BYTES) {
      if (!file.active) {
        return std::unexpected(Status::Corruption("archived WAL segment has a torn header"));
      }
      const auto encoded = std::as_bytes(std::span{bytes});
      const auto prefix_bytes = std::min(encoded.size(), wal_format::MAGIC.size());
      const auto magic_prefix =
          std::ranges::equal(encoded.first(prefix_bytes), std::span{wal_format::MAGIC}.first(prefix_bytes));
      const auto zero = std::ranges::all_of(encoded, [](std::byte byte) { return byte == std::byte{0}; });
      if (!magic_prefix && !zero) {
        return std::unexpected(Status::UnsupportedFormat("unrecognized TinyDB WAL prefix"));
      }
      log.active_partial = true;
      continue;
    }

    const auto header = wal_format::DecodeHeader(std::as_bytes(std::span{bytes}).first(wal_format::HEADER_BYTES));
    if (!header) {
      return std::unexpected(header.error());
    }
    if (!file.active && file.segment_id != header->segment_id) {
      return std::unexpected(Status::Corruption("WAL archive name disagrees with its segment header"));
    }
    log.segments.push_back(LoadedSegment{.file = std::move(file), .header = *header, .bytes = std::move(bytes)});
  }

  if (log.segments.empty()) {
    // The caller may clear this non-durable active-header remnant without a
    // database base.  It contains no complete identity or transaction.
    return std::optional{std::move(log)};
  }

  std::ranges::sort(log.segments, {}, [](const LoadedSegment &segment) { return segment.header.segment_id; });
  log.database_uuid = log.segments.front().header.database_uuid;
  for (std::size_t index = 0; index < log.segments.size(); ++index) {
    const auto &segment = log.segments[index];
    if (segment.header.database_uuid != log.database_uuid ||
        (index != 0 && segment.header.segment_id <= log.segments[index - 1].header.segment_id)) {
      return std::unexpected(Status::Corruption("WAL segment identity sequence is duplicated or reordered"));
    }
    if (segment.file.active && index + 1 != log.segments.size()) {
      return std::unexpected(Status::Corruption("active WAL segment is not the newest segment"));
    }
  }

  log.cleanup_needed = log.active_partial || log.segments.size() > 1;
  for (std::size_t segment_index = 0; segment_index < log.segments.size(); ++segment_index) {
    auto &segment = log.segments[segment_index];
    log.active_present = log.active_present || segment.file.active;

    // Transactions never cross a segment, so record identity restarts at the
    // authenticated segment header.  BuildPlan later decides whether a gap
    // between two independently valid segments is covered by the checkpoint
    // or corrupts the still-live suffix.
    auto expected_lsn = segment.header.starting_lsn;
    auto expected_sequence = std::uint32_t{0};
    auto offset = std::size_t{wal_format::HEADER_BYTES};
    auto last_type = std::optional<wal_format::RecordType>{};
    auto run_offset = offset;
    auto run_first_lsn = expected_lsn;
    while (offset < segment.bytes.size()) {
      const auto remaining = std::as_bytes(std::span{segment.bytes}).subspan(offset);
      const auto total = storage::GetLittleEndian<std::uint32_t>(remaining, 0);
      if (!total || *total < wal_format::RECORD_HEADER_BYTES || *total > MAX_RECORD_BYTES ||
          *total > remaining.size()) {
        if (!segment.file.active || segment_index + 1 != log.segments.size()) {
          return std::unexpected(Status::Corruption("archived WAL segment has a torn record"));
        }
        log.cleanup_needed = true;
        break;
      }

      const auto encoded_record = remaining.first(*total);
      const auto record = wal_format::DecodeRecord(encoded_record);
      if (!record) {
        const auto torn_checksum = record.error().Message() == "WAL record checksum mismatch" && segment.file.active &&
                                   segment_index + 1 == log.segments.size() && offset + *total == segment.bytes.size();
        if (torn_checksum) {
          log.cleanup_needed = true;
          break;
        }
        return std::unexpected(record.error());
      }
      if (record->lsn != expected_lsn || record->record_sequence != expected_sequence) {
        return std::unexpected(Status::Corruption("WAL record sequence is missing, duplicated, or reordered"));
      }
      if (expected_lsn == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(Status::Corruption("WAL record sequence overflows the LSN space"));
      }

      if (expected_sequence == 0) {
        run_offset = offset;
        run_first_lsn = record->lsn;
      }
      ++expected_lsn;
      last_type = record->type;
      if (record->type == wal_format::RecordType::Commit) {
        const auto run = std::as_bytes(std::span{segment.bytes}).subspan(run_offset, offset + *total - run_offset);
        auto transaction = wal_format::DecodeTransaction(run, run_first_lsn);
        if (!transaction) {
          return std::unexpected(transaction.error());
        }
        log.transactions.push_back(std::move(*transaction));
        expected_sequence = 0;
      } else {
        if (expected_sequence == std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(Status::Corruption("WAL transaction record sequence overflows"));
        }
        ++expected_sequence;
      }
      offset += *total;
    }
    if (segment.bytes.size() != wal_format::HEADER_BYTES) {
      log.cleanup_needed = true;
    }
    const auto has_following_segment = segment_index + 1 != log.segments.size() || log.active_partial;
    if (has_following_segment && last_type != wal_format::RecordType::Commit) {
      return std::unexpected(Status::Corruption("a WAL transaction crosses a segment boundary"));
    }
    if (last_type.has_value() && last_type != wal_format::RecordType::Commit && !segment.file.active) {
      return std::unexpected(Status::Corruption("archived WAL ends with an incomplete transaction"));
    }
    // Decoded page images now own every complete transaction. Release the
    // segment buffer immediately instead of retaining a second full WAL copy
    // through semantic planning and redo.
    segment.bytes = std::vector<char>{};
  }
  log.cleanup_needed = log.cleanup_needed || !log.active_present;
  return std::optional{std::move(log)};
}

/*
** Select the checkpoint base from untrusted database bytes.  Absence is
** represented only for a genuinely empty/new file.  A recognized but damaged
** pair is corruption: WAL is a suffix and cannot establish which historical
** base pages are missing.
*/
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
      .transaction_id = transaction.transaction_id,
      .root_page_id = transaction.state.root_page_id,
      .allocator_root_page_id = transaction.state.allocator_root_page_id,
      .high_water_page_id = transaction.state.high_water_page_id,
      .required_features = base.value.required_features,
      .optional_features = base.value.optional_features,
  });
}

/*
** Derive redo metadata from the complete transactions authenticated while
** loading the segments. This semantic half of Pass 1 must finish successfully
** before Recover opens the database read/write.
*/
auto BuildPlan(LoadedLog log, const storage::SelectedSuperblock &base) -> Result<RecoveryPlan> {
  if (base.value.database_uuid != log.database_uuid) {
    return std::unexpected(Status::InvalidArgument("write-ahead log does not belong to this database"));
  }
  if (base.value.checkpoint_lsn == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("checkpoint LSN space exhausted"));
  }

  auto plan = RecoveryPlan{
      .database_uuid = log.database_uuid,
      .transactions = {},
      .recovered_superblock = std::nullopt,
      .superblock_page_id = SUPERBLOCK_A_PAGE_ID,
      .archive_paths = {},
      .active_segment_id = log.segments.back().header.segment_id,
      .durable_next_lsn = std::max(log.segments.front().header.starting_lsn, base.value.checkpoint_lsn + 1U),
      .active_present = log.active_present,
      .cleanup_needed = log.cleanup_needed,
  };
  if (!plan.active_present) {
    if (plan.active_segment_id == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(Status::ResourceExhausted("WAL segment ID space exhausted during recovery"));
    }
    ++plan.active_segment_id;
  }
  for (const auto &segment : log.segments) {
    if (!segment.file.active) {
      plan.archive_paths.push_back(segment.file.path);
    }
  }

  const auto checkpoint_lsn = base.value.checkpoint_lsn;
  auto previous_transaction_id = std::uint64_t{0};
  auto previous_next_lsn = std::uint64_t{0};
  auto has_previous_transaction = false;
  auto saw_uncheckpointed_transaction = false;
  auto expected_live_lsn = checkpoint_lsn + 1U;
  auto expected_live_transaction_id =
      base.value.transaction_id == std::numeric_limits<std::uint64_t>::max() ? 0 : base.value.transaction_id + 1U;
  auto previous_high_water = base.value.high_water_page_id;
  for (auto &transaction : log.transactions) {
    if (has_previous_transaction) {
      if (transaction.first_lsn != previous_next_lsn) {
        // Cleanup may have removed any subset of segments whose records are
        // already represented by the selected superblock. Such a gap is safe
        // only while both sides remain at or behind checkpoint_lsn. The first
        // live record and everything after it must be exactly contiguous.
        if (transaction.first_lsn < previous_next_lsn || previous_next_lsn > checkpoint_lsn + 1U ||
            transaction.first_lsn > checkpoint_lsn + 1U) {
          return std::unexpected(Status::Corruption("WAL transaction LSN sequence is missing or reordered"));
        }
      }
    }
    if (has_previous_transaction) {
      if (transaction.transaction_id <= previous_transaction_id) {
        return std::unexpected(Status::Corruption("WAL transaction ID sequence is duplicated or reordered"));
      }
    }
    previous_transaction_id = transaction.transaction_id;
    previous_next_lsn = transaction.next_lsn;
    has_previous_transaction = true;
    plan.durable_next_lsn = std::max(plan.durable_next_lsn, transaction.next_lsn);

    if (transaction.commit_lsn > checkpoint_lsn) {
      if (transaction.first_lsn <= checkpoint_lsn) {
        return std::unexpected(Status::Corruption("database checkpoint cuts through a WAL transaction"));
      }
      if (expected_live_transaction_id == 0 || transaction.first_lsn != expected_live_lsn ||
          transaction.transaction_id != expected_live_transaction_id) {
        return std::unexpected(Status::Corruption("WAL live transaction suffix is missing or reordered"));
      }
      saw_uncheckpointed_transaction = true;
      expected_live_lsn = transaction.next_lsn;
      if (expected_live_transaction_id == std::numeric_limits<std::uint64_t>::max()) {
        expected_live_transaction_id = 0;
      } else {
        ++expected_live_transaction_id;
      }
      // A transaction may have begun while this checkpoint was writing and
      // therefore record an older allocator-reuse frontier. It may never claim
      // a checkpoint newer than the selected durable base.
      if (transaction.state.checkpoint_lsn > checkpoint_lsn ||
          transaction.state.high_water_page_id < previous_high_water ||
          transaction.state.high_water_page_id > MAX_FILE_PAGES) {
        return std::unexpected(Status::Corruption("WAL database-state frontier is inconsistent with its base"));
      }
      for (const auto &page : transaction.pages) {
        if (page.page_id >= transaction.state.high_water_page_id) {
          return std::unexpected(Status::Corruption("WAL page image lies beyond its allocation frontier"));
        }
      }
      previous_high_water = transaction.state.high_water_page_id;
      plan.transactions.push_back(std::move(transaction));
    } else if (transaction.transaction_id > base.value.transaction_id) {
      return std::unexpected(Status::Corruption("covered WAL transaction is newer than the selected superblock"));
    }
  }
  if (!saw_uncheckpointed_transaction && log.segments.front().header.starting_lsn > checkpoint_lsn + 1U) {
    return std::unexpected(Status::Corruption("WAL segment sequence begins after the checkpoint frontier"));
  }
  if (!saw_uncheckpointed_transaction && log.active_present &&
      log.segments.back().header.starting_lsn > checkpoint_lsn + 1U) {
    return std::unexpected(Status::Corruption("active WAL begins after the checkpoint frontier"));
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
** Pass 2 consumes an already validated plan.  Data pages are synchronized
** before the inactive superblock can advertise them.  No WAL byte is removed
** here; cleanup is a separate final phase and therefore cannot deprive a
** failed redo of its recovery basis.
*/
auto Redo(const std::filesystem::path &db_path, const RecoveryPlan &plan) -> Status {
  if (plan.transactions.empty()) {
    return {};
  }
  TINYDB_CHECK(plan.recovered_superblock.has_value(), "recovery plan with transactions has no superblock");
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

  if (auto status = io::FullPwrite(fd.Get(), plan.recovered_superblock->data(), plan.recovered_superblock->size(),
                                   plan.superblock_page_id * PAGE_SIZE);
      !status.Ok()) {
    return status;
  }
  if (io::Fsync(fd.Get()) < 0) {
    return io::ErrnoStatus("fsync recovered superblock");
  }
  return {};
}

/*
** Replace the active tail only after a covering superblock is durable (or no
** redo was required).  The clean header preserves the global LSN and segment
** frontiers.  Archive deletion is followed by one directory fsync; failure
** leaves redundant covered history but never removes needed redo.
*/
auto CleanupWal(const std::filesystem::path &wal_path, const RecoveryPlan &plan) -> Status {
  if (!plan.cleanup_needed) {
    return {};
  }
  auto active_fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!active_fd.Valid() || io::Ftruncate(active_fd.Get(), 0) < 0) {
    return io::ErrnoStatus("replace active WAL segment");
  }
  auto header = wal_format::EncodeHeader(wal_format::Header{
      .database_uuid = plan.database_uuid,
      .segment_id = plan.active_segment_id,
      .starting_lsn = plan.durable_next_lsn,
  });
  if (!header) {
    return header.error();
  }
  if (auto status = io::FullPwrite(active_fd.Get(), header->data(), header->size(), 0); !status.Ok()) {
    return status;
  }
  if (io::Fsync(active_fd.Get()) < 0) {
    return io::ErrnoStatus("fsync clean WAL segment");
  }

  for (const auto &archive : plan.archive_paths) {
    if (io::Unlink(archive) < 0 && errno != ENOENT) {
      return io::ErrnoStatus("remove checkpointed WAL segment");
    }
  }
  if (!plan.archive_paths.empty() || !plan.active_present) {
    return io::SyncParentDirectory(wal_path);
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
  if (!loaded.has_value()) {
    return {};
  }
  auto log = std::move(loaded.value());
  if (log.segments.empty()) {
    // A partial active header contains no authenticated database identity or
    // commit.  Clearing it cannot discard acknowledged state; Wal::Open will
    // create and synchronize a complete header after the database is opened.
    auto fd = UniqueFd(io::Open(wal_path, O_RDWR | O_CLOEXEC));
    if (!fd.Valid() || io::Ftruncate(fd.Get(), 0) < 0) {
      return io::ErrnoStatus("clear incomplete WAL header");
    }
    return {};
  }

  auto base_result = ReadDatabaseBase(db_path);
  if (!base_result) {
    return base_result.error();
  }
  auto base = *base_result;
  if (!base.has_value()) {
    return Status::Corruption("database base is missing; WAL is not a complete backup");
  }

  // BuildPlan is the write-free boundary.  Once it returns, all segment,
  // transaction, page, state, and superblock bytes needed by redo are valid.
  auto plan = BuildPlan(std::move(log), base.value());
  if (!plan) {
    return plan.error();
  }
  if (auto status = Redo(db_path, *plan); !status.Ok()) {
    return status;
  }
  return CleanupWal(wal_path, *plan);
}

}  // namespace tinydb::recovery
