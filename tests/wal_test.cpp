#include <gtest/gtest.h>
#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"
#include "recovery/recovery.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "txn/database_state.h"
#include "wal/wal_codec.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

/*
** WAL tests treat record framing and syscall ordering as part of the durable
** contract. They construct complete and torn runs, inject failures at append,
** sync, database redo, and truncation boundaries, and require recovery to
** preserve a valid committed prefix without accepting corrupt middle records.
*/
static constexpr std::uint64_t WAL_HEADER_BYTES = tinydb::wal_format::HEADER_BYTES;
static constexpr std::uint64_t PAGE_IMAGE_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + sizeof(tinydb::page_id_t) + tinydb::PAGE_SIZE;
static constexpr std::uint64_t DATABASE_STATE_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + tinydb::wal_format::DATABASE_STATE_PAYLOAD_BYTES;
static constexpr std::uint64_t COMMIT_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + tinydb::wal_format::COMMIT_PAYLOAD_BYTES;
static constexpr std::uint64_t ONE_PAGE_TRANSACTION_BYTES =
    PAGE_IMAGE_RECORD_BYTES + DATABASE_STATE_RECORD_BYTES + COMMIT_RECORD_BYTES;

// Keep expected record geometry derived from public format constants, but not
// from Wal::SizeBytes itself. These assertions catch accidental framing growth
// that would otherwise remain internally self-consistent.

// The database identity this suite stamps into its files and logs; tests
// about mismatched pairs use a different value on one side.
static auto Uuid(std::byte last) -> tinydb::DatabaseUuid {
  auto uuid = tinydb::DatabaseUuid{};
  uuid.back() = last;
  return uuid;
}

static const auto TEST_UUID = Uuid(std::byte{1});
static const auto OTHER_UUID = Uuid(std::byte{2});

static auto DataPage(tinydb::page_id_t page_id, char marker) -> std::array<char, tinydb::PAGE_SIZE> {
  // WAL recovery now validates every page image before replay. Use a minimal
  // real encoded page rather than arbitrary marker-filled bytes so tests reach
  // the WAL behavior they intend to exercise.
  const auto payload = std::array{static_cast<std::byte>(marker)};
  const auto page = tinydb::storage::EncodeOverflowPage(page_id, 0, payload.size(), tinydb::HEADER_PAGE_ID, payload);
  EXPECT_TRUE(page.has_value());
  return page.value();
}

static auto State(tinydb::page_id_t high_water = 4,
                  tinydb::page_id_t root = tinydb::HEADER_PAGE_ID) -> tinydb::txn::DatabaseState {
  return tinydb::txn::DatabaseState{
      .root_page_id = root,
      .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
      .high_water_page_id = high_water,
      .transaction_id = 0,
      .visible_lsn = 0,
      .checkpoint_lsn = 0,
  };
}

static auto Commit(tinydb::Wal &wal, tinydb::page_id_t high_water = 4,
                   tinydb::page_id_t root = tinydb::HEADER_PAGE_ID) -> tinydb::Result<tinydb::Wal::CommitInfo> {
  return wal.Commit(State(high_water, root));
}

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_" + name + "_" + std::to_string(::getpid()) + ".db");
}

class ScopedSyscallHook {
 public:
  // Always remove the process-global hook, including when an ASSERT exits the
  // current test early.
  explicit ScopedSyscallHook(tinydb::io::TestHook hook) { tinydb::io::SetTestHook(std::move(hook)); }
  ScopedSyscallHook(const ScopedSyscallHook &) = delete;
  auto operator=(const ScopedSyscallHook &) -> ScopedSyscallHook & = delete;
  ~ScopedSyscallHook() { tinydb::io::ClearTestHook(); }
};

static auto ParentPath(const std::filesystem::path &path) -> std::filesystem::path {
  const auto parent = path.parent_path();
  return parent.empty() ? std::filesystem::path{"."} : parent;
}

static auto FindCall(const std::vector<tinydb::io::Call> &calls, tinydb::io::Syscall syscall,
                     const std::filesystem::path &path, std::size_t start = 0) -> std::size_t {
  for (auto i = start; i < calls.size(); ++i) {
    if (calls[i].syscall == syscall && calls[i].path == path) {
      return i;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

// Creates a fully codec-valid database of `pages` pages. Using encoded pages is
// essential now that recovery refuses to replay/checkpoint opaque junk.
static void MakeDbFile(const std::filesystem::path &path, int pages) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  const auto superblock = tinydb::storage::EncodeSuperblock(tinydb::storage::Superblock{
      .database_uuid = TEST_UUID,
      .root_page_id = tinydb::HEADER_PAGE_ID,
      .high_water_page_id = static_cast<tinydb::page_id_t>(pages),
  });
  ASSERT_TRUE(superblock.has_value());
  file.write(reinterpret_cast<const char *>(superblock->data()), static_cast<std::streamsize>(superblock->size()));
  file.write(reinterpret_cast<const char *>(superblock->data()), static_cast<std::streamsize>(superblock->size()));
  for (int i = static_cast<int>(tinydb::FIRST_DATA_PAGE_ID); i < pages; ++i) {
    const auto page = tinydb::storage::EncodeOverflowPage(
        static_cast<tinydb::page_id_t>(i), 0, 1, tinydb::HEADER_PAGE_ID, std::as_bytes(std::span{"x", std::size_t{1}}));
    ASSERT_TRUE(page.has_value());
    file.write(page->data(), static_cast<std::streamsize>(page->size()));
  }
}

static auto ReadWholeFile(const std::filesystem::path &path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void FlipByteAt(const std::filesystem::path &path, std::uint64_t offset) {
  // Preserve record length and file geometry so the checksum path, not simple
  // truncation handling, decides whether the location is a torn tail or durable
  // middle corruption.
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.get(byte);
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(byte ^ 0x1));
}

static void BreakTransactionDigestButKeepRecordChecksum(const std::filesystem::path &path,
                                                        std::uint64_t commit_offset) {
  /*
  ** A raw bit flip is rejected while segment framing is scanned.  Re-encoding
  ** the changed COMMIT record preserves its record checksum while breaking
  ** the digest over preceding transaction records.  Recovery can discover
  ** this defect only when it authenticates the complete transaction, which
  ** makes it the regression case for the write-free first pass.
  */
  auto file_bytes = ReadWholeFile(path);
  auto record = tinydb::wal_format::DecodeRecord(
      std::as_bytes(std::span{file_bytes}).subspan(commit_offset, COMMIT_RECORD_BYTES));
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->type, tinydb::wal_format::RecordType::Commit);
  record->payload[tinydb::wal_format::COMMIT_TRANSACTION_DIGEST_OFFSET] ^= std::byte{1};
  auto encoded = tinydb::wal_format::EncodeRecord(record->type, record->transaction_id, record->lsn,
                                                  record->record_sequence, record->payload);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_EQ(encoded->size(), COMMIT_RECORD_BYTES);

  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(static_cast<std::streamoff>(commit_offset));
  file.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
  ASSERT_TRUE(file.good());
}

TEST(WalTest, PathForAppendsWalSuffix) {
  EXPECT_EQ(tinydb::Wal::PathFor("/tmp/data.db"), std::filesystem::path("/tmp/data.db-wal"));
}

TEST(WalTest, OpenStampsAndKeepsTheMagic) {
  const auto path = TestPath("wal_open");
  std::filesystem::remove(path);

  {
    auto wal = tinydb::Wal::Open(path, TEST_UUID).value();
    EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  }
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES);

  // Reopen: the magic checks out and nothing is rewritten.
  EXPECT_TRUE(tinydb::Wal::Open(path, TEST_UUID).has_value());

  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, OpenFsyncsFreshLogBeforeParentDirectory) {
  const auto path = TestPath("wal_open_order");
  std::filesystem::remove(path);

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    auto wal = tinydb::Wal::Open(path, TEST_UUID);
    ASSERT_TRUE(wal.has_value());
  }

  const auto magic_write = FindCall(calls, tinydb::io::Syscall::Pwrite, path);
  const auto wal_sync = FindCall(calls, tinydb::io::Syscall::Fsync, path, magic_write + 1);
  const auto parent_sync = FindCall(calls, tinydb::io::Syscall::Fsync, ParentPath(path), wal_sync + 1);

  ASSERT_NE(magic_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(parent_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(magic_write, wal_sync);
  EXPECT_LT(wal_sync, parent_sync);

  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, OpenFailsIfFreshLogDirectoryFsyncFails) {
  const auto path = TestPath("wal_open_parent_fsync_fails");
  std::filesystem::remove(path);

  auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Fsync && call.path == ParentPath(path)) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};

  const auto wal = tinydb::Wal::Open(path, TEST_UUID);
  ASSERT_FALSE(wal.has_value());
  EXPECT_EQ(wal.error().Code(), tinydb::StatusCode::IoError);

  std::filesystem::remove(path);
}

TEST(WalTest, OpenRejectsAForeignFile) {
  const auto path = TestPath("wal_foreign");
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "definitely not a wal";
  }

  const auto wal = tinydb::Wal::Open(path, TEST_UUID);
  ASSERT_FALSE(wal.has_value());
  EXPECT_EQ(wal.error().Code(), tinydb::StatusCode::UnsupportedFormat);

  std::filesystem::remove(path);
}

TEST(WalTest, OpenRejectsALogFromAnotherDatabase) {
  const auto path = TestPath("wal_wrong_uuid");
  std::filesystem::remove(path);

  { ASSERT_TRUE(tinydb::Wal::Open(path, TEST_UUID).has_value()); }

  // The magic checks out but the UUID does not: this log belongs to some
  // other database, and appending to it would corrupt that one's history.
  const auto wal = tinydb::Wal::Open(path, OTHER_UUID);
  ASSERT_FALSE(wal.has_value());
  EXPECT_EQ(wal.error().Code(), tinydb::StatusCode::InvalidArgument);

  std::filesystem::remove(path);
}

TEST(WalTest, CheckpointCoverageDropsLogicalPressureWithoutRewritingActiveTail) {
  const auto path = TestPath("wal_commit_reset");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  const auto first = DataPage(2, 'a');
  const auto second = DataPage(3, 'a');
  wal.AppendPageImage(2, first.data());
  wal.AppendPageImage(3, second.data());
  const auto committed = Commit(wal);
  ASSERT_TRUE(committed.has_value());

  const auto expected =
      WAL_HEADER_BYTES + (2 * PAGE_IMAGE_RECORD_BYTES) + DATABASE_STATE_RECORD_BYTES + COMMIT_RECORD_BYTES;
  EXPECT_EQ(wal.SizeBytes(), expected);
  EXPECT_EQ(std::filesystem::file_size(path), expected);

  ASSERT_TRUE(wal.CleanupCheckpointed(committed->commit_lsn).Ok());
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  EXPECT_EQ(std::filesystem::file_size(path), expected);

  std::filesystem::remove(path);
}

TEST(WalTest, RotationKeepsEachTransactionInsideOneOrderedSegment) {
  const auto path = TestPath("wal_segment_rotation");
  const auto archive = tinydb::Wal::SegmentPathFor(path, 1);
  std::filesystem::remove(path);
  std::filesystem::remove(archive);
  auto wal = tinydb::Wal::Open(path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES).value();

  const auto first = DataPage(2, 'a');
  wal.AppendPageImage(2, first.data());
  ASSERT_TRUE(Commit(wal).has_value());
  EXPECT_FALSE(std::filesystem::exists(archive));

  const auto second = DataPage(3, 'b');
  wal.AppendPageImage(3, second.data());
  const auto committed = Commit(wal);
  ASSERT_TRUE(committed.has_value());
  EXPECT_TRUE(std::filesystem::exists(archive));
  EXPECT_EQ(std::filesystem::file_size(archive), WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES);
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES);
  EXPECT_EQ(wal.SizeBytes(), 2 * (WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES));

  const auto archived = ReadWholeFile(archive);
  const auto active = ReadWholeFile(path);
  const auto archived_header =
      tinydb::wal_format::DecodeHeader(std::as_bytes(std::span{archived.data(), tinydb::wal_format::HEADER_BYTES}));
  const auto active_header =
      tinydb::wal_format::DecodeHeader(std::as_bytes(std::span{active.data(), tinydb::wal_format::HEADER_BYTES}));
  ASSERT_TRUE(archived_header.has_value());
  ASSERT_TRUE(active_header.has_value());
  EXPECT_EQ(archived_header->segment_id, 1U);
  EXPECT_EQ(active_header->segment_id, 2U);
  EXPECT_EQ(active_header->starting_lsn, committed->commit_lsn - 2U);

  ASSERT_TRUE(wal.CleanupCheckpointed(committed->commit_lsn).Ok());
  EXPECT_FALSE(std::filesystem::exists(archive));
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  std::filesystem::remove(path);
}

TEST(WalTest, CoveredArchiveCleanupFailureLeavesLiveAppendingAndIsRetryable) {
  const auto path = TestPath("wal_checkpoint_cleanup_retry");
  const auto archive = tinydb::Wal::SegmentPathFor(path, 1);
  std::filesystem::remove(path);
  std::filesystem::remove(archive);
  auto wal = tinydb::Wal::Open(path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES).value();

  wal.AppendPageImage(2, DataPage(2, 'a').data());
  ASSERT_TRUE(Commit(wal).has_value());
  wal.AppendPageImage(3, DataPage(3, 'b').data());
  const auto second = Commit(wal);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(std::filesystem::exists(archive));

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Unlink && call.path == archive) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    EXPECT_EQ(wal.CleanupCheckpointed(second->commit_lsn).Code(), tinydb::StatusCode::IoError);
  }

  // Cleanup is maintenance: the immutable covered segment remains harmless,
  // and failure does not poison the active append target.
  EXPECT_TRUE(std::filesystem::exists(archive));
  ASSERT_TRUE(wal.CleanupCheckpointed(second->commit_lsn).Ok());
  EXPECT_FALSE(std::filesystem::exists(archive));
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  std::filesystem::remove(path);
}

TEST(WalTest, RecoveryReplaysAndRemovesAnOrderedSegmentSequence) {
  const auto db_path = TestPath("recover_segments");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  const auto archive = tinydb::Wal::SegmentPathFor(wal_path, 1);
  std::filesystem::remove(wal_path);
  std::filesystem::remove(archive);
  MakeDbFile(db_path, 4);
  const auto first = DataPage(2, 'a');
  const auto second = DataPage(3, 'b');
  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES).value();
    wal.AppendPageImage(2, first.data());
    ASSERT_TRUE(Commit(wal).has_value());
    wal.AppendPageImage(3, second.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }
  ASSERT_TRUE(std::filesystem::exists(archive));

  ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  const auto database = ReadWholeFile(db_path);
  EXPECT_EQ(database.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(first.data(), first.size()));
  EXPECT_EQ(database.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(second.data(), second.size()));
  EXPECT_FALSE(std::filesystem::exists(archive));
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoveryRejectsAMissingSegmentAfterTheCheckpoint) {
  const auto db_path = TestPath("recover_missing_segment");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  const auto first_archive = tinydb::Wal::SegmentPathFor(wal_path, 1);
  const auto second_archive = tinydb::Wal::SegmentPathFor(wal_path, 2);
  std::filesystem::remove(wal_path);
  std::filesystem::remove(first_archive);
  std::filesystem::remove(second_archive);
  MakeDbFile(db_path, 5);
  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES).value();
    for (tinydb::page_id_t page_id = 2; page_id < 5; ++page_id) {
      const auto page = DataPage(page_id, static_cast<char>('a' + page_id));
      wal.AppendPageImage(page_id, page.data());
      ASSERT_TRUE(Commit(wal, 5).has_value());
    }
  }
  ASSERT_TRUE(std::filesystem::exists(first_archive));
  ASSERT_TRUE(std::filesystem::exists(second_archive));
  std::filesystem::remove(first_archive);

  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
  std::filesystem::remove(second_archive);
}

TEST(WalDurabilityTest, RotationSynchronizesArchiveAndNewActiveDirectoryEntriesBeforeAppend) {
  const auto path = TestPath("wal_segment_rotation_order");
  const auto archive = tinydb::Wal::SegmentPathFor(path, 1);
  std::filesystem::remove(path);
  std::filesystem::remove(archive);
  auto wal = tinydb::Wal::Open(path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES).value();
  const auto page = DataPage(2, 'a');
  wal.AppendPageImage(2, page.data());
  ASSERT_TRUE(Commit(wal).has_value());

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    wal.AppendPageImage(3, DataPage(3, 'b').data());
    ASSERT_TRUE(Commit(wal).has_value());
  }

  const auto renamed = FindCall(calls, tinydb::io::Syscall::Rename, path);
  const auto archive_dir_sync = FindCall(calls, tinydb::io::Syscall::Fsync, ParentPath(path), renamed + 1);
  const auto create_active = FindCall(calls, tinydb::io::Syscall::Open, path, archive_dir_sync + 1);
  const auto header_write = FindCall(calls, tinydb::io::Syscall::Pwrite, path, create_active + 1);
  const auto header_sync = FindCall(calls, tinydb::io::Syscall::Fsync, path, header_write + 1);
  const auto active_dir_sync = FindCall(calls, tinydb::io::Syscall::Fsync, ParentPath(path), header_sync + 1);
  const auto transaction_write = FindCall(calls, tinydb::io::Syscall::Pwrite, path, active_dir_sync + 1);
  EXPECT_LT(renamed, archive_dir_sync);
  EXPECT_LT(archive_dir_sync, create_active);
  EXPECT_LT(create_active, header_write);
  EXPECT_LT(header_write, header_sync);
  EXPECT_LT(header_sync, active_dir_sync);
  EXPECT_LT(active_dir_sync, transaction_write);

  ASSERT_TRUE(wal.CleanupCheckpointed(6).Ok());
  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, IndeterminateRotationDirectorySyncRequiresRecovery) {
  const auto path = TestPath("wal_segment_rotation_sync_fails");
  const auto archive = tinydb::Wal::SegmentPathFor(path, 1);
  std::filesystem::remove(path);
  std::filesystem::remove(archive);
  auto wal = tinydb::Wal::Open(path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES).value();
  const auto first = DataPage(2, 'a');
  wal.AppendPageImage(2, first.data());
  ASSERT_TRUE(Commit(wal).has_value());

  auto renamed = false;
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Rename && call.path == path) {
        renamed = true;
      } else if (renamed && call.syscall == tinydb::io::Syscall::Fsync && call.path == ParentPath(path)) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto second = DataPage(3, 'b');
    wal.AppendPageImage(3, second.data());
    const auto failed = Commit(wal);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().Code(), tinydb::StatusCode::NeedsRecovery);
  }
  EXPECT_TRUE(renamed);

  const auto second = DataPage(3, 'b');
  wal.AppendPageImage(3, second.data());
  const auto refused = Commit(wal);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().Code(), tinydb::StatusCode::NeedsRecovery);

  std::filesystem::remove(path);
  std::filesystem::remove(archive);
}

TEST(WalTest, DiscardPendingDropsTheBufferedImages) {
  const auto path = TestPath("wal_discard");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  const auto first = DataPage(2, 'x');
  wal.AppendPageImage(2, first.data());
  wal.DiscardPending();

  // Only the post-discard image reaches the file.
  const auto second = DataPage(3, 'x');
  wal.AppendPageImage(3, second.data());
  ASSERT_TRUE(Commit(wal).has_value());
  EXPECT_EQ(wal.SizeBytes(),
            WAL_HEADER_BYTES + PAGE_IMAGE_RECORD_BYTES + DATABASE_STATE_RECORD_BYTES + COMMIT_RECORD_BYTES);

  std::filesystem::remove(path);
}

TEST(WalTest, CommitRejectsAPreassignedTransactionIdBeforeAppend) {
  const auto path = TestPath("wal_transaction_frontier");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  const auto page = DataPage(2, 'x');
  wal.AppendPageImage(2, page.data());
  auto state = State();
  state.transaction_id = 2;
  const auto result = wal.Commit(state);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES);

  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, CommitDoesNotAdvanceTailWhenWalFsyncFails) {
  const auto path = TestPath("wal_commit_fsync_fails");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  const auto image = DataPage(2, 'x');
  wal.AppendPageImage(2, image.data());

  auto calls = std::vector<tinydb::io::Call>{};
  auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    calls.push_back(call);
    if (call.syscall == tinydb::io::Syscall::Fsync && call.path == path) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};

  const auto status = Commit(wal);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().Code(), tinydb::StatusCode::IndeterminateCommit);
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);

  const auto append = FindCall(calls, tinydb::io::Syscall::Pwrite, path);
  const auto sync = FindCall(calls, tinydb::io::Syscall::Fsync, path, append + 1);
  ASSERT_NE(append, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(sync, std::numeric_limits<std::size_t>::max());

  tinydb::io::ClearTestHook();
  wal.AppendPageImage(2, image.data());
  const auto refused = Commit(wal);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().Code(), tinydb::StatusCode::NeedsRecovery);

  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, AppendFailureRepairsTheKnownGoodTailAndPermitsAnotherCommit) {
  const auto path = TestPath("wal_append_repair");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();
  const auto image = DataPage(2, 'x');
  wal.AppendPageImage(2, image.data());

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == path) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto failed = Commit(wal);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().Code(), tinydb::StatusCode::IoError);
  }
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES);

  wal.AppendPageImage(2, image.data());
  EXPECT_TRUE(Commit(wal).has_value());
  EXPECT_GT(wal.SizeBytes(), WAL_HEADER_BYTES);
  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, FailedTailRepairRequiresRecovery) {
  const auto path = TestPath("wal_append_repair_fails");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();
  const auto image = DataPage(2, 'x');
  wal.AppendPageImage(2, image.data());

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if ((call.syscall == tinydb::io::Syscall::Pwrite || call.syscall == tinydb::io::Syscall::Ftruncate) &&
          call.path == path) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto failed = Commit(wal);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().Code(), tinydb::StatusCode::NeedsRecovery);
  }

  wal.AppendPageImage(2, image.data());
  const auto refused = Commit(wal);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().Code(), tinydb::StatusCode::NeedsRecovery);
  std::filesystem::remove(path);
}

TEST(WalTest, RecoverIsANoOpWithoutALog) {
  const auto db_path = TestPath("recover_no_log");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);

  EXPECT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  EXPECT_FALSE(std::filesystem::exists(wal_path));
}

TEST(WalTest, RecoverAppliesCommittedRuns) {
  const auto db_path = TestPath("recover_apply");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 4);
  const auto expected_second = DataPage(2, 'b');
  const auto expected_third = DataPage(3, 'b');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto first = DataPage(2, 'a');
    wal.AppendPageImage(2, first.data());
    ASSERT_TRUE(Commit(wal).has_value());
    wal.AppendPageImage(2, expected_second.data());  // the later run wins on the same page
    wal.AppendPageImage(3, expected_third.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }

  ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 4 * tinydb::PAGE_SIZE);
  const auto page_a = std::as_bytes(std::span{db.data(), tinydb::PAGE_SIZE});
  const auto page_b = std::as_bytes(std::span{db.data() + tinydb::PAGE_SIZE, tinydb::PAGE_SIZE});
  const auto recovered_state = tinydb::storage::SelectSuperblock(page_a, page_b);
  ASSERT_TRUE(recovered_state.has_value());
  EXPECT_EQ(recovered_state->slot, tinydb::storage::SuperblockSlot::B);
  EXPECT_EQ(recovered_state->value.database_uuid, TEST_UUID);
  EXPECT_EQ(recovered_state->value.generation, 2U);
  EXPECT_EQ(recovered_state->value.transaction_id, 2U);
  EXPECT_EQ(recovered_state->value.checkpoint_lsn, 7U);

  // Recovery advances only the inactive slot.  Until B is durable, A remains
  // the complete previous recovery basis; rewriting both would destroy that
  // atomic generation switch.
  const auto previous_state = tinydb::storage::DecodeSuperblock(page_a);
  ASSERT_TRUE(previous_state.has_value());
  EXPECT_EQ(previous_state->generation, 1U);
  EXPECT_EQ(previous_state->checkpoint_lsn, 0U);
  EXPECT_EQ(db.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(expected_second.data(), expected_second.size()));
  EXPECT_EQ(db.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(expected_third.data(), expected_third.size()));

  // The replayed log is empty again, down to its magic.
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  // Idempotent: a second pass has nothing to do and changes nothing.
  EXPECT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  EXPECT_EQ(ReadWholeFile(db_path), db);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDurabilityTest, RecoverSyncsDatabaseBeforeTruncatingWal) {
  const auto db_path = TestPath("recover_order");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 3);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto image = DataPage(2, 'r');
    wal.AppendPageImage(2, image.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  }

  const auto db_write = FindCall(calls, tinydb::io::Syscall::Pwrite, db_path);
  const auto db_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path, db_write + 1);
  const auto wal_truncate = FindCall(calls, tinydb::io::Syscall::Ftruncate, wal_path, db_sync + 1);
  const auto wal_sync = FindCall(calls, tinydb::io::Syscall::Fsync, wal_path, wal_truncate + 1);

  ASSERT_NE(db_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(db_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_truncate, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(db_write, db_sync);
  EXPECT_LT(db_sync, wal_truncate);
  EXPECT_LT(wal_truncate, wal_sync);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDurabilityTest, RecoverLeavesWalIntactWhenDatabaseFsyncFails) {
  const auto db_path = TestPath("recover_db_fsync_fails");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 3);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto image = DataPage(2, 'f');
    wal.AppendPageImage(2, image.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Fsync && call.path == db_path) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};

  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::IoError);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRefusesToTreatWalAsABackupForAMissingDatabase) {
  const auto db_path = TestPath("recover_missing_db");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);

  const auto leaf_page = DataPage(2, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(2, leaf_page.data());
    ASSERT_TRUE(Commit(wal, 3).has_value());
  }

  const auto wal_before = ReadWholeFile(wal_path);
  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_FALSE(std::filesystem::exists(db_path));
  EXPECT_EQ(ReadWholeFile(wal_path), wal_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDurabilityTest, RecoverSyncsPagesAndAlternateSuperblockBeforeCleaningWal) {
  const auto db_path = TestPath("recover_two_database_syncs");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 3);

  const auto leaf_page = DataPage(2, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(2, leaf_page.data());
    ASSERT_TRUE(Commit(wal, 3).has_value());
  }

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  }

  const auto page_write = FindCall(calls, tinydb::io::Syscall::Pwrite, db_path);
  const auto page_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path, page_write + 1);
  const auto superblock_write = FindCall(calls, tinydb::io::Syscall::Pwrite, db_path, page_sync + 1);
  const auto superblock_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path, superblock_write + 1);
  const auto wal_truncate = FindCall(calls, tinydb::io::Syscall::Ftruncate, wal_path, superblock_sync + 1);
  const auto wal_sync = FindCall(calls, tinydb::io::Syscall::Fsync, wal_path, wal_truncate + 1);

  ASSERT_NE(page_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(page_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(superblock_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(superblock_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_truncate, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(page_write, page_sync);
  EXPECT_LT(page_sync, superblock_write);
  EXPECT_LT(superblock_write, superblock_sync);
  EXPECT_LT(superblock_sync, wal_truncate);
  EXPECT_LT(wal_truncate, wal_sync);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverDiscardsATornTail) {
  const auto db_path = TestPath("recover_torn");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 4);
  const auto db_before = ReadWholeFile(db_path);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto first = DataPage(2, 'a');
    const auto second = DataPage(2, 'c');
    const auto third = DataPage(3, 'c');
    wal.AppendPageImage(2, first.data());
    ASSERT_TRUE(Commit(wal).has_value());
    wal.AppendPageImage(2, second.data());
    wal.AppendPageImage(3, third.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }

  // Chop the log mid-way through the second run's commit record: its images
  // are intact, but the run never reached its durability point.
  const auto full_size = std::filesystem::file_size(wal_path);
  std::filesystem::resize_file(wal_path, full_size - (COMMIT_RECORD_BYTES - 4));

  ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  EXPECT_EQ(db.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(DataPage(2, 'a').data(), tinydb::PAGE_SIZE));
  EXPECT_EQ(db.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            db_before.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE));
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRejectsCorruptionInTheDurableMiddle) {
  const auto db_path = TestPath("recover_bad_crc");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 4);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto first = DataPage(2, 'a');
    const auto second = DataPage(3, 'c');
    wal.AppendPageImage(2, first.data());
    ASSERT_TRUE(Commit(wal).has_value());
    wal.AppendPageImage(3, second.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }

  // A damaged record followed by more durable records is not a torn tail;
  // recovery must report corruption and preserve the WAL for diagnosis.
  const auto second_run_start =
      WAL_HEADER_BYTES + PAGE_IMAGE_RECORD_BYTES + DATABASE_STATE_RECORD_BYTES + COMMIT_RECORD_BYTES;
  FlipByteAt(wal_path, second_run_start + 100);

  const auto wal_size = std::filesystem::file_size(wal_path);
  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, TransactionValidationFinishesBeforeTheFirstDatabaseWrite) {
  const auto db_path = TestPath("recover_validate_before_redo");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 4);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto first = DataPage(2, 'a');
    const auto second = DataPage(3, 'b');
    wal.AppendPageImage(2, first.data());
    ASSERT_TRUE(Commit(wal).has_value());
    wal.AppendPageImage(3, second.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }

  // The second COMMIT remains a well-framed, checksummed record, but no
  // longer authenticates its preceding PAGE_IMAGE and DATABASE_STATE.
  const auto second_transaction = WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES;
  const auto second_commit = second_transaction + PAGE_IMAGE_RECORD_BYTES + DATABASE_STATE_RECORD_BYTES;
  BreakTransactionDigestButKeepRecordChecksum(wal_path, second_commit);
  const auto database_before = ReadWholeFile(db_path);
  const auto wal_before = ReadWholeFile(wal_path);

  auto database_writes = std::size_t{0};
  auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == db_path) {
      ++database_writes;
    }
    return std::nullopt;
  }};
  const auto status = tinydb::recovery::Recover(db_path, wal_path);

  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(database_writes, 0U);
  EXPECT_EQ(ReadWholeFile(db_path), database_before);
  EXPECT_EQ(ReadWholeFile(wal_path), wal_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverClearsATornHeader) {
  const auto db_path = TestPath("recover_torn_header");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  {
    // A crash between creating the log and fsyncing its magic can leave a
    // short remnant; recovery clears it so the next Open() starts fresh.
    std::ofstream file(wal_path, std::ios::binary | std::ios::trunc);
    file << "TI";
  }

  ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  EXPECT_EQ(std::filesystem::file_size(wal_path), 0U);
  EXPECT_TRUE(tinydb::Wal::Open(wal_path, TEST_UUID).has_value());

  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverClearsAnIncompleteHeader) {
  const auto db_path = TestPath("recover_incomplete_header");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);

  // Only the beginning of the header landed before creation crashed. It
  // never held a record, so recovery clears it.
  { ASSERT_TRUE(tinydb::Wal::Open(wal_path, TEST_UUID).has_value()); }
  std::filesystem::resize_file(wal_path, 6);

  ASSERT_TRUE(tinydb::recovery::Recover(db_path, wal_path).Ok());
  EXPECT_EQ(std::filesystem::file_size(wal_path), 0U);
  EXPECT_TRUE(tinydb::Wal::Open(wal_path, TEST_UUID).has_value());

  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRejectsAForeignFile) {
  const auto db_path = TestPath("recover_foreign");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  {
    std::ofstream file(wal_path, std::ios::binary | std::ios::trunc);
    file << "definitely not a wal";
  }

  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::UnsupportedFormat);
  // The file it refused to trust is left exactly as it was.
  EXPECT_EQ(ReadWholeFile(wal_path), "definitely not a wal");

  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRejectsAForeignDatabaseBeforeReplay) {
  const auto db_path = TestPath("recover_foreign_db");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  {
    std::ofstream file(db_path, std::ios::binary | std::ios::trunc);
    file << "this is not a tinydb database";
  }
  const auto db_before = ReadWholeFile(db_path);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto image = DataPage(2, 'x');
    wal.AppendPageImage(2, image.data());
    ASSERT_TRUE(Commit(wal, 3).has_value());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::UnsupportedFormat);
  EXPECT_EQ(ReadWholeFile(db_path), db_before);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRefusesAWalFromAnotherDatabase) {
  const auto db_path = TestPath("recover_wrong_uuid");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 2);
  const auto db_before = ReadWholeFile(db_path);

  // A committed run in a log stamped with a different database's UUID.
  {
    auto wal = tinydb::Wal::Open(wal_path, OTHER_UUID).value();
    const auto image = DataPage(2, 'x');
    wal.AppendPageImage(2, image.data());
    ASSERT_TRUE(Commit(wal).has_value());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  // The database's superblock is intact and vouches for its UUID, so the
  // mismatched log is refused before a single image is replayed.
  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(ReadWholeFile(db_path), db_before);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRefusesToGuessABaseWhenBothSuperblocksAreDamaged) {
  const auto db_path = TestPath("recover_torn_db_header");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 3);

  const auto data_page = DataPage(2, 'r');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(2, data_page.data());
    ASSERT_TRUE(Commit(wal, 3).has_value());
  }

  // Tear the on-disk header the way a crashed in-place rewrite would: the
  // magic survives, but a checksummed field (root_page_id) changes, so the
  // checksum no longer holds.
  FlipByteAt(db_path, tinydb::storage::superblock_offset::ROOT_PAGE_ID);
  FlipByteAt(db_path, tinydb::PAGE_SIZE + tinydb::storage::superblock_offset::ROOT_PAGE_ID);
  const auto database_before = ReadWholeFile(db_path);
  const auto wal_before = ReadWholeFile(wal_path);

  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(ReadWholeFile(db_path), database_before);
  EXPECT_EQ(ReadWholeFile(wal_path), wal_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRefusesAZeroedBaseEvenWhenWalContainsPageImages) {
  const auto db_path = TestPath("recover_zeroed_header");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  {
    // The database file exists, but its fresh header never reached the disk:
    // nothing in it but zeroes.
    std::ofstream file(db_path, std::ios::binary | std::ios::trunc);
    const auto zeroes = std::string(2 * tinydb::PAGE_SIZE, '\0');
    file.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
  }

  const auto leaf_page = DataPage(2, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(2, leaf_page.data());
    ASSERT_TRUE(Commit(wal, 3).has_value());
  }
  const auto database_before = ReadWholeFile(db_path);
  const auto wal_before = ReadWholeFile(wal_path);

  const auto status = tinydb::recovery::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(ReadWholeFile(db_path), database_before);
  EXPECT_EQ(ReadWholeFile(wal_path), wal_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDeathTest, CommitWithNothingBufferedAborts) {
  const auto path = TestPath("wal_empty_commit");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  EXPECT_DEATH(
      {
        auto status = Commit(wal);
        static_cast<void>(status);
      },
      "logged no page images");

  std::filesystem::remove(path);
}
