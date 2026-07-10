#include <gtest/gtest.h>
#include <tinydb/file_header.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"
#include "util/checksums.h"

#include <unistd.h>

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

// On-disk sizes, mirrored from the format documented in wal.h:
// header = {u32 magic}, record = {u32 length | u32 crc32 | payload}.
static constexpr std::uint64_t WAL_HEADER_BYTES = 4;
static constexpr std::uint64_t PAGE_IMAGE_RECORD_BYTES = 8 + 1 + 8 + tinydb::PAGE_SIZE;
static constexpr std::uint64_t COMMIT_RECORD_BYTES = 8 + 1;

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_" + name + "_" + std::to_string(::getpid()) + ".db");
}

class ScopedSyscallHook {
 public:
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

// A database file of `pages` pages, page i filled with the byte '0' + i.
static void MakeDbFile(const std::filesystem::path &path, int pages) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  auto header_page = std::string(tinydb::PAGE_SIZE, '0');
  auto header = tinydb::FileHeader{
      .magic = tinydb::TINYDB_FILE_MAGIC,
      .page_size = tinydb::PAGE_SIZE,
      .root_page_id = tinydb::HEADER_PAGE_ID,
      .next_page_id = static_cast<tinydb::page_id_t>(pages),
      .free_list_head = tinydb::HEADER_PAGE_ID,
      .checksum = 0,
  };
  header.checksum = tinydb::HeaderChecksum(header);
  std::memcpy(header_page.data(), &header, sizeof(header));
  file.write(header_page.data(), static_cast<std::streamsize>(header_page.size()));
  for (int i = 0; i < pages; ++i) {
    if (i == 0) {
      continue;
    }
    const std::string page(tinydb::PAGE_SIZE, static_cast<char>('0' + i));
    file.write(page.data(), static_cast<std::streamsize>(page.size()));
  }
}

static auto ReadWholeFile(const std::filesystem::path &path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void FlipByteAt(const std::filesystem::path &path, std::uint64_t offset) {
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
    auto wal = tinydb::Wal::Open(path).value();
    EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  }
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES);

  // Reopen: the magic checks out and nothing is rewritten.
  EXPECT_TRUE(tinydb::Wal::Open(path).has_value());

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
    auto wal = tinydb::Wal::Open(path);
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

  const auto wal = tinydb::Wal::Open(path);
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

  const auto wal = tinydb::Wal::Open(path);
  ASSERT_FALSE(wal.has_value());
  EXPECT_EQ(wal.error().Code(), tinydb::StatusCode::InvalidArgument);

  std::filesystem::remove(path);
}

TEST(WalTest, CommitGrowsTheLogAndResetEmptiesIt) {
  const auto path = TestPath("wal_commit_reset");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path).value();

  const std::string image(tinydb::PAGE_SIZE, 'a');
  wal.AppendPageImage(1, image.data());
  wal.AppendPageImage(2, image.data());
  ASSERT_TRUE(wal.Commit().Ok());

  const auto expected = WAL_HEADER_BYTES + (2 * PAGE_IMAGE_RECORD_BYTES) + COMMIT_RECORD_BYTES;
  EXPECT_EQ(wal.SizeBytes(), expected);
  EXPECT_EQ(std::filesystem::file_size(path), expected);

  ASSERT_TRUE(wal.Reset().Ok());
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES);
  EXPECT_EQ(std::filesystem::file_size(path), WAL_HEADER_BYTES);

  // The magic survives a reset: the file still opens as a log.
  EXPECT_TRUE(tinydb::Wal::Open(path).has_value());

  std::filesystem::remove(path);
}

TEST(WalTest, DiscardPendingDropsTheBufferedImages) {
  const auto path = TestPath("wal_discard");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path).value();

  const std::string image(tinydb::PAGE_SIZE, 'x');
  wal.AppendPageImage(1, image.data());
  wal.DiscardPending();

  // Only the post-discard image reaches the file.
  wal.AppendPageImage(2, image.data());
  ASSERT_TRUE(wal.Commit().Ok());
  EXPECT_EQ(wal.SizeBytes(), WAL_HEADER_BYTES + PAGE_IMAGE_RECORD_BYTES + COMMIT_RECORD_BYTES);

  std::filesystem::remove(path);
}

TEST(WalDurabilityTest, CommitDoesNotAdvanceTailWhenWalFsyncFails) {
  const auto path = TestPath("wal_commit_fsync_fails");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path).value();

  const auto image = std::string(tinydb::PAGE_SIZE, 'x');
  wal.AppendPageImage(1, image.data());

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
  MakeDbFile(db_path, 3);
  const auto db_before = ReadWholeFile(db_path);

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    const std::string first(tinydb::PAGE_SIZE, 'a');
    const std::string second(tinydb::PAGE_SIZE, 'b');
    wal.AppendPageImage(1, first.data());
    ASSERT_TRUE(wal.Commit().Ok());
    wal.AppendPageImage(1, second.data());  // the later run wins on the same page
    wal.AppendPageImage(2, second.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 3 * tinydb::PAGE_SIZE);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), db_before.substr(0, tinydb::PAGE_SIZE));  // header page untouched
  EXPECT_EQ(db[tinydb::PAGE_SIZE], 'b');
  EXPECT_EQ(db[(2 * tinydb::PAGE_SIZE) - 1], 'b');
  EXPECT_EQ(db[2 * tinydb::PAGE_SIZE], 'b');

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
  MakeDbFile(db_path, 2);

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    const auto image = std::string(tinydb::PAGE_SIZE, 'r');
    wal.AppendPageImage(1, image.data());
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
  MakeDbFile(db_path, 2);

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    const auto image = std::string(tinydb::PAGE_SIZE, 'f');
    wal.AppendPageImage(1, image.data());
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

  auto header_page = std::string(tinydb::PAGE_SIZE, '\0');
  auto header = tinydb::FileHeader{
      .magic = tinydb::TINYDB_FILE_MAGIC,
      .page_size = tinydb::PAGE_SIZE,
      .root_page_id = 1,
      .next_page_id = 2,
      .free_list_head = tinydb::HEADER_PAGE_ID,
      .checksum = 0,
  };
  header.checksum = tinydb::HeaderChecksum(header);
  std::memcpy(header_page.data(), &header, sizeof(header));
  const auto leaf_page = std::string(tinydb::PAGE_SIZE, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    wal.AppendPageImage(0, header_page.data());
    wal.AppendPageImage(1, leaf_page.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 2 * tinydb::PAGE_SIZE);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), header_page);
  EXPECT_EQ(db.substr(tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), leaf_page);
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDurabilityTest, RecoverSyncsCreatedDatabaseDirectoryBeforeTruncatingWal) {
  const auto db_path = TestPath("recover_created_db_order");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);

  auto header_page = std::string(tinydb::PAGE_SIZE, '\0');
  auto header = tinydb::FileHeader{
      .magic = tinydb::TINYDB_FILE_MAGIC,
      .page_size = tinydb::PAGE_SIZE,
      .root_page_id = 1,
      .next_page_id = 2,
      .free_list_head = tinydb::HEADER_PAGE_ID,
      .checksum = 0,
  };
  header.checksum = tinydb::HeaderChecksum(header);
  std::memcpy(header_page.data(), &header, sizeof(header));

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    wal.AppendPageImage(0, header_page.data());
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
  MakeDbFile(db_path, 3);

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    const std::string first(tinydb::PAGE_SIZE, 'a');
    const std::string second(tinydb::PAGE_SIZE, 'c');
    wal.AppendPageImage(1, first.data());
    ASSERT_TRUE(wal.Commit().Ok());
    wal.AppendPageImage(1, second.data());
    wal.AppendPageImage(2, second.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  // Chop the log mid-way through the second run's commit record: its images
  // are intact, but the run never reached its durability point.
  const auto full_size = std::filesystem::file_size(wal_path);
  std::filesystem::resize_file(wal_path, full_size - (COMMIT_RECORD_BYTES - 4));

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  EXPECT_EQ(db[tinydb::PAGE_SIZE], 'a');      // only the first run applied
  EXPECT_EQ(db[2 * tinydb::PAGE_SIZE], '2');  // page 2 untouched
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalTest, RecoverStopsAtACorruptedRecord) {
  const auto db_path = TestPath("recover_bad_crc");
  const auto wal_path = tinydb::Wal::PathFor(db_path);
  std::filesystem::remove(wal_path);
  MakeDbFile(db_path, 3);

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    const std::string first(tinydb::PAGE_SIZE, 'a');
    const std::string second(tinydb::PAGE_SIZE, 'c');
    wal.AppendPageImage(1, first.data());
    ASSERT_TRUE(wal.Commit().Ok());
    wal.AppendPageImage(2, second.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  // Flip one byte inside the second run's page image: its CRC no longer
  // matches, so the scan stops there and the run is discarded — commit
  // record and all.
  const auto second_run_start = WAL_HEADER_BYTES + PAGE_IMAGE_RECORD_BYTES + COMMIT_RECORD_BYTES;
  FlipByteAt(wal_path, second_run_start + 100);

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  EXPECT_EQ(db[tinydb::PAGE_SIZE], 'a');
  EXPECT_EQ(db[2 * tinydb::PAGE_SIZE], '2');
  EXPECT_EQ(std::filesystem::file_size(wal_path), WAL_HEADER_BYTES);

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
    file << "TD";
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());
  EXPECT_EQ(std::filesystem::file_size(wal_path), 0U);
  EXPECT_TRUE(tinydb::Wal::Open(wal_path).has_value());

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
  EXPECT_EQ(status.Code(), tinydb::StatusCode::InvalidArgument);
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
    auto wal = tinydb::Wal::Open(wal_path).value();
    const std::string image(tinydb::PAGE_SIZE, 'x');
    wal.AppendPageImage(0, image.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }
  const auto wal_size_before = std::filesystem::file_size(wal_path);

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
  MakeDbFile(db_path, 2);

  // A committed run carrying a header image, the way the engine logs one
  // whenever an operation changes the header.
  auto header_page = std::string(tinydb::PAGE_SIZE, '\0');
  auto header = tinydb::FileHeader{
      .magic = tinydb::TINYDB_FILE_MAGIC,
      .page_size = tinydb::PAGE_SIZE,
      .root_page_id = 1,
      .next_page_id = 2,
      .free_list_head = tinydb::HEADER_PAGE_ID,
      .checksum = 0,
  };
  header.checksum = tinydb::HeaderChecksum(header);
  std::memcpy(header_page.data(), &header, sizeof(header));

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    wal.AppendPageImage(0, header_page.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  // Tear the on-disk header the way a crashed in-place rewrite would: the
  // magic survives, but a checksummed field (root_page_id) changes, so the
  // checksum no longer holds.
  FlipByteAt(db_path, 8);

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), header_page);
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
    const auto zeroes = std::string(tinydb::PAGE_SIZE, '\0');
    file.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
  }

  auto header_page = std::string(tinydb::PAGE_SIZE, '\0');
  auto header = tinydb::FileHeader{
      .magic = tinydb::TINYDB_FILE_MAGIC,
      .page_size = tinydb::PAGE_SIZE,
      .root_page_id = 1,
      .next_page_id = 2,
      .free_list_head = tinydb::HEADER_PAGE_ID,
      .checksum = 0,
  };
  header.checksum = tinydb::HeaderChecksum(header);
  std::memcpy(header_page.data(), &header, sizeof(header));
  const auto leaf_page = std::string(tinydb::PAGE_SIZE, 'l');

  {
    auto wal = tinydb::Wal::Open(wal_path).value();
    wal.AppendPageImage(0, header_page.data());
    wal.AppendPageImage(1, leaf_page.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }

  ASSERT_TRUE(tinydb::Wal::Recover(db_path, wal_path).Ok());

  const auto db = ReadWholeFile(db_path);
  ASSERT_EQ(db.size(), 2 * tinydb::PAGE_SIZE);
  EXPECT_EQ(db.substr(0, tinydb::PAGE_SIZE), header_page);
  EXPECT_EQ(db.substr(tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), leaf_page);

  std::filesystem::remove(db_path);
  std::filesystem::remove(wal_path);
}

TEST(WalDeathTest, CommitWithNothingBufferedAborts) {
  const auto path = TestPath("wal_empty_commit");
  std::filesystem::remove(path);
  auto wal = tinydb::Wal::Open(path).value();

  EXPECT_DEATH(
      {
        auto status = wal.Commit();
        static_cast<void>(status);
      },
      "logged no page images");

  std::filesystem::remove(path);
}
