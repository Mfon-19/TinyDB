#include <gtest/gtest.h>
#include <tinydb/database_uuid.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
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
#include <string>
#include <utility>
#include <vector>

static constexpr std::uint64_t WAL_HEADER_BYTES = tinydb::wal_format::HEADER_BYTES;
static constexpr std::uint64_t PAGE_IMAGE_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + sizeof(tinydb::page_id_t) + tinydb::PAGE_SIZE;
static constexpr std::uint64_t COMMIT_RECORD_BYTES = tinydb::wal_format::RECORD_HEADER_BYTES + 16;

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

static auto SuperblockPage(const tinydb::DatabaseUuid &uuid = TEST_UUID, std::uint64_t generation = 2,
                           tinydb::page_id_t high_water = tinydb::FIRST_DATA_PAGE_ID)
    -> std::array<char, tinydb::PAGE_SIZE> {
  // Superblock images exercise the metadata half of physical redo; generation
  // and frontier are parameters because recovery tests need both fresh and
  // advanced database states.
  const auto page = tinydb::storage::EncodeSuperblock(tinydb::storage::Superblock{
      .database_uuid = uuid,
      .generation = generation,
      .high_water_page_id = high_water,
  });
  EXPECT_TRUE(page.has_value());
  auto output = std::array<char, tinydb::PAGE_SIZE>{};
  std::memcpy(output.data(), page->data(), page->size());
  return output;
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

TEST(WalTest, CommitGrowsTheLogAndResetEmptiesIt) {
  const auto path = TestPath("wal_commit_reset");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  const auto first = DataPage(2, 'a');
  const auto second = DataPage(3, 'a');
  wal.AppendPageImage(2, first.data());
  wal.AppendPageImage(3, second.data());
  ASSERT_TRUE(wal.Commit().Ok());

  const auto expected = WAL_HEADER_BYTES + (2 * PAGE_IMAGE_RECORD_BYTES) + COMMIT_RECORD_BYTES;
  EXPECT_EQ(wal.SizeBytes(), expected);
  EXPECT_EQ(std::filesystem::file_size(path), expected);

  ASSERT_TRUE(wal.Reset().Ok());
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES);

  // The magic survives a reset: the file still opens as a log.
  EXPECT_TRUE(tinydb::Wal::Open(path, TEST_UUID).has_value());

  std::filesystem::remove(path);
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
  ASSERT_TRUE(wal.Commit().Ok());
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES + PAGE_IMAGE_RECORD_BYTES + COMMIT_RECORD_BYTES);

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

  const auto status = wal.Commit();
  EXPECT_EQ(status.Code(), tinydb::StatusCode::IoError);
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);

  const auto append = FindCall(calls, tinydb::io::Syscall::Pwrite, path);
  const auto sync = FindCall(calls, tinydb::io::Syscall::Fsync, path, append + 1);
  ASSERT_NE(append, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(sync, std::numeric_limits<std::size_t>::max());

  std::filesystem::remove(path);
}

TEST(WalTest, RecoverIsANoOpWithoutALog) {
  const auto db_path = TestPath("recover_no_log");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);

  EXPECT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
  EXPECT_FALSE(std::filesystem::exists(wal_path));
}

TEST(WalTest, RecoverAppliesCommittedRuns) {
  const auto db_path = TestPath("recover_apply");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 4);
  const auto db_before = ReadWholeFile(db_path);
  const auto expected_second = DataPage(2, 'b');
  const auto expected_third = DataPage(3, 'b');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    const auto first = DataPage(2, 'a');
    wal.AppendPageImage(2, first.data());
    ASSERT_TRUE(wal.Commit().Ok());
    wal.AppendPageImage(2, expected_second.data());  // the later run wins on the same page
    wal.AppendPageImage(3, expected_third.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 4 * tinydb::PAGE_SIZE);
  EXPECT_EQ(db.substr(0, 2 * tinydb::PAGE_SIZE), db_before.substr(0, 2 * tinydb::PAGE_SIZE));
  EXPECT_EQ(db.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(expected_second.data(), expected_second.size()));
  EXPECT_EQ(db.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(expected_third.data(), expected_third.size()));

  // The replayed log is empty again, down to its magic.
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  // Idempotent: a second pass has nothing to do and changes nothing.
  EXPECT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
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
    ASSERT_TRUE(wal.Commit().Ok());
  }

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
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
    ASSERT_TRUE(wal.Commit().Ok());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Fsync && call.path == db_path) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};

  const auto status = tinydb::Wal::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::IoError);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverCanRebuildMissingDatabaseFromCommittedImages) {
  const auto db_path = TestPath("recover_missing_db");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);

  const auto superblock_a = SuperblockPage(TEST_UUID, 1, 3);
  const auto superblock_b = superblock_a;
  const auto leaf_page = DataPage(2, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(0, superblock_a.data());
    wal.AppendPageImage(1, superblock_b.data());
    wal.AppendPageImage(2, leaf_page.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 3 * tinydb::PAGE_SIZE);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), std::string(superblock_a.data(), superblock_a.size()));
  EXPECT_EQ(db.substr(tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(superblock_b.data(), superblock_b.size()));
  EXPECT_EQ(db.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(leaf_page.data(), leaf_page.size()));
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDurabilityTest, RecoverSyncsCreatedDatabaseDirectoryBeforeTruncatingWal) {
  const auto db_path = TestPath("recover_created_db_order");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);

  const auto superblock_a = SuperblockPage(TEST_UUID, 1);
  const auto superblock_b = superblock_a;

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(0, superblock_a.data());
    wal.AppendPageImage(1, superblock_b.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
  }

  const auto db_write = FindCall(calls, tinydb::io::Syscall::Pwrite, db_path);
  const auto db_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path, db_write + 1);
  const auto db_parent_sync = FindCall(calls, tinydb::io::Syscall::Fsync, ParentPath(db_path), db_sync + 1);
  const auto wal_truncate = FindCall(calls, tinydb::io::Syscall::Ftruncate, wal_path, db_parent_sync + 1);
  const auto wal_sync = FindCall(calls, tinydb::io::Syscall::Fsync, wal_path, wal_truncate + 1);

  ASSERT_NE(db_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(db_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(db_parent_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_truncate, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(db_write, db_sync);
  EXPECT_LT(db_sync, db_parent_sync);
  EXPECT_LT(db_parent_sync, wal_truncate);
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
    ASSERT_TRUE(wal.Commit().Ok());
    wal.AppendPageImage(2, second.data());
    wal.AppendPageImage(3, third.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  // Chop the log mid-way through the second run's commit record: its images
  // are intact, but the run never reached its durability point.
  const auto full_size = std::filesystem::file_size(wal_path);
  std::filesystem::resize_file(wal_path, full_size - (COMMIT_RECORD_BYTES - 4));

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

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
    ASSERT_TRUE(wal.Commit().Ok());
    wal.AppendPageImage(3, second.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  // A damaged record followed by more durable records is not a torn tail;
  // recovery must report corruption and preserve the WAL for diagnosis.
  const auto second_run_start = WAL_HEADER_BYTES + PAGE_IMAGE_RECORD_BYTES + COMMIT_RECORD_BYTES;
  FlipByteAt(wal_path, second_run_start + 100);

  const auto wal_size = std::filesystem::file_size(wal_path);
  const auto status = tinydb::Wal::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size);

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

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
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

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
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

  const auto status = tinydb::Wal::Recover(db_path, wal_path);
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
    const auto image = SuperblockPage();
    wal.AppendPageImage(0, image.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  const auto status = tinydb::Wal::Recover(db_path, wal_path);
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
    ASSERT_TRUE(wal.Commit().Ok());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  // The database's superblock is intact and vouches for its UUID, so the
  // mismatched log is refused before a single image is replayed.
  const auto status = tinydb::Wal::Recover(db_path, wal_path);
  EXPECT_EQ(status.Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(ReadWholeFile(db_path), db_before);
  EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRepairsATornDatabaseHeader) {
  const auto db_path = TestPath("recover_torn_db_header");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 3);

  // A committed run carrying a header image, the way the engine logs one
  // whenever an operation changes the header.
  const auto header_page = SuperblockPage(TEST_UUID, 2, 3);

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(0, header_page.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  // Tear the on-disk header the way a crashed in-place rewrite would: the
  // magic survives, but a checksummed field (root_page_id) changes, so the
  // checksum no longer holds.
  FlipByteAt(db_path, tinydb::storage::superblock_offset::ROOT_PAGE_ID);

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), std::string(header_page.data(), header_page.size()));
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverRepairsAZeroedHeaderFromACrashedCreation) {
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

  const auto header_page = SuperblockPage(TEST_UUID, 1, 3);
  const auto second_header_page = header_page;
  const auto leaf_page = DataPage(2, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path, TEST_UUID).value();
    wal.AppendPageImage(0, header_page.data());
    wal.AppendPageImage(1, second_header_page.data());
    wal.AppendPageImage(2, leaf_page.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 3 * tinydb::PAGE_SIZE);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), std::string(header_page.data(), header_page.size()));
  EXPECT_EQ(db.substr(tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(second_header_page.data(), second_header_page.size()));
  EXPECT_EQ(db.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(leaf_page.data(), leaf_page.size()));

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDeathTest, CommitWithNothingBufferedAborts) {
  const auto path = TestPath("wal_empty_commit");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path, TEST_UUID).value();

  EXPECT_DEATH(
      {
        auto status = wal.Commit();
        static_cast<void>(status);
      },
      "logged no page images");

  std::filesystem::remove(path);
}
