#include <gtest/gtest.h>
#include <tinydb/status.h>
#include <tinydb/storage_engine.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"

#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

// Scan end bound past every key this suite generates.
constexpr const char *SCAN_END = "\x7f";

auto RowKey(int row) -> std::string {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "row-%06d", row);
  return std::string{buffer};
}

auto RowValue(int row, std::size_t length) -> std::string {
  auto value = std::string(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    value[i] = static_cast<char>('a' + (static_cast<std::size_t>(row) + i) % 26);
  }
  return value;
}

class ScopedSyscallHook {
 public:
  explicit ScopedSyscallHook(tinydb::io::TestHook hook) { tinydb::io::SetTestHook(std::move(hook)); }
  ScopedSyscallHook(const ScopedSyscallHook &) = delete;
  auto operator=(const ScopedSyscallHook &) -> ScopedSyscallHook & = delete;
  ~ScopedSyscallHook() { tinydb::io::ClearTestHook(); }
};

auto FindCall(const std::vector<tinydb::io::Call> &calls, tinydb::io::Syscall syscall,
              const std::filesystem::path &path, std::size_t start = 0) -> std::size_t {
  for (auto i = start; i < calls.size(); ++i) {
    if (calls[i].syscall == syscall && calls[i].path == path) {
      return i;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

class StorageEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto stem = "tinydb_engine_" + std::string(info->name()) + "_" + std::to_string(::getpid());
    db_path_ = std::filesystem::temp_directory_path() / (stem + ".db");
    second_db_path_ = std::filesystem::temp_directory_path() / (stem + "_b.db");
    RemoveDatabase(db_path_);
    RemoveDatabase(second_db_path_);
  }

  void TearDown() override {
    RemoveDatabase(db_path_);
    RemoveDatabase(second_db_path_);
  }

  // A database on disk is the file plus its write-ahead log.
  static void RemoveDatabase(const std::filesystem::path &path) {
    std::filesystem::remove(path);
    std::filesystem::remove(tinydb::Wal::PathFor(path));
  }

  // Copies the database and its log as they sit on disk right now — the
  // exact state a crash would leave behind (nothing flushed, nothing
  // closed) — so a second engine can be opened on the copy while the
  // original stays live.
  void SnapshotDatabase() const {
    std::filesystem::copy_file(db_path_, second_db_path_);
    std::filesystem::copy_file(tinydb::Wal::PathFor(db_path_), tinydb::Wal::PathFor(second_db_path_));
  }

  std::filesystem::path db_path_;
  std::filesystem::path second_db_path_;
};

TEST_F(StorageEngineTest, OpenCreatesEmptyDatabase) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  EXPECT_TRUE(std::filesystem::exists(db_path_));
  EXPECT_EQ(engine.Get("anything").value(), std::nullopt);
  EXPECT_TRUE(engine.Scan("", SCAN_END).value().empty());
  EXPECT_TRUE(engine.Remove("anything").Ok());  // removing from an empty database is a no-op
}

TEST_F(StorageEngineTest, PutGetRemoveRoundTrip) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  EXPECT_TRUE(engine.Put("apple", "red").Ok());
  EXPECT_TRUE(engine.Put("banana", "yellow").Ok());

  EXPECT_EQ(engine.Get("apple").value(), std::optional<std::string>{"red"});
  EXPECT_EQ(engine.Get("banana").value(), std::optional<std::string>{"yellow"});

  EXPECT_TRUE(engine.Remove("apple").Ok());
  EXPECT_EQ(engine.Get("apple").value(), std::nullopt);
  EXPECT_EQ(engine.Get("banana").value(), std::optional<std::string>{"yellow"});
}

TEST_F(StorageEngineTest, PutReplacesExistingValue) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  EXPECT_TRUE(engine.Put("key", "first").Ok());
  EXPECT_TRUE(engine.Put("key", "second").Ok());

  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{"second"});
  EXPECT_EQ(engine.Scan("", SCAN_END).value().size(), 1);
}

TEST_F(StorageEngineTest, EntrySizeCapIsEnforced) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  // Exactly at the cap is accepted.
  const auto key = std::string{"key"};
  const auto max_value = RowValue(0, tinydb::MAX_ENTRY_BYTES - key.size());
  EXPECT_TRUE(engine.Put(key, max_value).Ok());

  // One byte over is rejected and must not disturb existing data.
  const auto oversized = max_value + "x";
  EXPECT_EQ(engine.Put("other", oversized).Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(engine.Put(key, oversized).Code(), tinydb::StatusCode::InvalidArgument);

  EXPECT_EQ(engine.Get("other").value(), std::nullopt);
  EXPECT_EQ(engine.Get(key).value(), std::optional<std::string>{max_value});
}

TEST_F(StorageEngineTest, ScanBoundsAreHalfOpen) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(engine.Put(RowKey(i), RowValue(i, 20)).Ok());
  }

  const auto rows = engine.Scan(RowKey(3), RowKey(6)).value();
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows.front().first, RowKey(3));
  EXPECT_EQ(rows.back().first, RowKey(5));

  EXPECT_TRUE(engine.Scan(RowKey(4), RowKey(4)).value().empty());
}

TEST_F(StorageEngineTest, ClosedHandleRefusesWork) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("key", "value").Ok());

  ASSERT_TRUE(engine.Close().Ok());

  EXPECT_EQ(engine.Put("key", "other").Code(), tinydb::StatusCode::Closed);
  EXPECT_EQ(engine.Remove("key").Code(), tinydb::StatusCode::Closed);
  const auto got = engine.Get("key");
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().Code(), tinydb::StatusCode::Closed);
  const auto rows = engine.Scan("", SCAN_END);
  ASSERT_FALSE(rows.has_value());
  EXPECT_EQ(rows.error().Code(), tinydb::StatusCode::Closed);
  EXPECT_TRUE(engine.Close().Ok());  // closing twice is safe

  // The pre-close write reached the file.
  auto reopened = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(reopened.Get("key").value(), std::optional<std::string>{"value"});
}

TEST_F(StorageEngineTest, DataSurvivesExplicitClose) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("persist", "me").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get("persist").value(), std::optional<std::string>{"me"});
}

TEST_F(StorageEngineTest, DataSurvivesDestructor) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("persist", "me too").Ok());
    // No Close(): the destructor must flush.
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get("persist").value(), std::optional<std::string>{"me too"});
}

TEST_F(StorageEngineTest, ReopenedDatabaseAcceptsMutations) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    for (int i = 0; i < 20; ++i) {
      ASSERT_TRUE(engine.Put(RowKey(i), RowValue(i, 30)).Ok());
    }
  }
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Remove(RowKey(5)).Ok());
    ASSERT_TRUE(engine.Put(RowKey(100), RowValue(100, 30)).Ok());
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get(RowKey(5)).value(), std::nullopt);
  EXPECT_EQ(engine.Get(RowKey(100)).value(), std::optional<std::string>{RowValue(100, 30)});
  EXPECT_EQ(engine.Scan("", SCAN_END).value().size(), 20);  // 20 - 1 + 1
}

TEST_F(StorageEngineTest, LargeWorkloadSurvivesReopen) {
  auto model = std::map<std::string, std::string>{};

  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    // Enough data for many leaf splits and at least one root split.
    for (int i = 0; i < 400; ++i) {
      const auto key = RowKey(i);
      const auto value = RowValue(i, 50 + (static_cast<std::size_t>(i) * 13) % 400);
      ASSERT_TRUE(engine.Put(key, value).Ok());
      model[key] = value;
    }
    for (int i = 0; i < 400; i += 3) {
      ASSERT_TRUE(engine.Remove(RowKey(i)).Ok());
      model.erase(RowKey(i));
    }
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  const auto rows = engine.Scan("", SCAN_END).value();
  ASSERT_EQ(rows.size(), model.size());
  auto it = model.begin();
  for (const auto &[key, value] : rows) {
    EXPECT_EQ(key, it->first);
    EXPECT_EQ(value, it->second);
    ++it;
  }
}

TEST_F(StorageEngineTest, ChurnDoesNotGrowTheFile) {
  const auto churn = [this] {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(engine.Put(RowKey(i), RowValue(i, 120)).Ok());
    }
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(engine.Remove(RowKey(i)).Ok());
    }
    ASSERT_TRUE(engine.Close().Ok());
  };

  // The first cycle sizes the file; merges and root collapses free the
  // emptied pages, so every later identical cycle runs on reused pages.
  churn();
  const auto stable_size = std::filesystem::file_size(db_path_);
  for (int cycle = 0; cycle < 3; ++cycle) {
    churn();
    EXPECT_EQ(std::filesystem::file_size(db_path_), stable_size);
  }
}

TEST_F(StorageEngineTest, MoveTransfersOwnership) {
  auto first = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(first.Put("moved", "data").Ok());

  auto second = std::move(first);

  // The moved-from handle acts closed.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(first.Put("x", "y").Code(), tinydb::StatusCode::Closed);

  // The destination owns the database.
  EXPECT_EQ(second.Get("moved").value(), std::optional<std::string>{"data"});
  EXPECT_TRUE(second.Put("more", "rows").Ok());
}

TEST_F(StorageEngineTest, MoveAssignClosesTheOldDatabase) {
  auto target = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(target.Put("old", "database").Ok());

  auto source = tinydb::StorageEngine::Open(second_db_path_).value();
  ASSERT_TRUE(source.Put("new", "database").Ok());

  target = std::move(source);

  // target now serves the second database...
  EXPECT_EQ(target.Get("new").value(), std::optional<std::string>{"database"});
  EXPECT_EQ(target.Get("old").value(), std::nullopt);

  // ...and the first database was flushed when target closed it.
  auto reopened = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(reopened.Get("old").value(), std::optional<std::string>{"database"});
}

TEST_F(StorageEngineTest, OpenRejectsForeignFiles) {
  {
    auto file = std::ofstream{db_path_};
    file << "this is not a tinydb database, just some text\n";
  }

  const auto result = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::InvalidArgument);
}

TEST_F(StorageEngineTest, OpenRejectsForeignFilesBeforeWalReplay) {
  {
    auto file = std::ofstream{db_path_, std::ios::binary | std::ios::trunc};
    file << "this is not a tinydb database, just some text\n";
  }
  const auto db_before = [&] {
    auto file = std::ifstream{db_path_, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }();

  {
    auto wal = tinydb::Wal::Open(tinydb::Wal::PathFor(db_path_)).value();
    const auto image = std::string(tinydb::PAGE_SIZE, 'x');
    wal.AppendPageImage(0, image.data());
    ASSERT_TRUE(wal.Commit().Ok());
  }
  const auto wal_size_before = std::filesystem::file_size(tinydb::Wal::PathFor(db_path_));

  const auto result = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::InvalidArgument);

  {
    auto file = std::ifstream{db_path_, std::ios::binary};
    const auto db_after = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(db_after, db_before);
  }
  EXPECT_EQ(std::filesystem::file_size(tinydb::Wal::PathFor(db_path_)), wal_size_before);
}

}  // namespace

TEST_F(StorageEngineTest, CommittedWritesSurviveACrash) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  for (int row = 0; row < 200; ++row) {
    ASSERT_TRUE(engine.Put(RowKey(row), RowValue(row, 64)).Ok());
  }
  for (int row = 0; row < 100; ++row) {
    ASSERT_TRUE(engine.Remove(RowKey(row)).Ok());
  }

  // "Crash": snapshot the files mid-flight, with the engine still open and
  // nothing checkpointed, and recover a second engine from the copy.
  SnapshotDatabase();
  auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();

  const auto rows = recovered.Scan("", SCAN_END).value();
  ASSERT_EQ(rows.size(), 100U);
  for (int row = 100; row < 200; ++row) {
    EXPECT_EQ(recovered.Get(RowKey(row)).value(), RowValue(row, 64));
  }
  EXPECT_EQ(recovered.Get(RowKey(0)).value(), std::nullopt);

  // Recovery leaves the copy a normal database: it accepts new work.
  ASSERT_TRUE(recovered.Put(RowKey(0), "back again").Ok());
  EXPECT_TRUE(recovered.Close().Ok());
}

TEST_F(StorageEngineTest, CleanCloseEmptiesTheLog) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("k", "v").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  // Close checkpointed: the log is down to its 4-byte header, and the
  // database file alone carries the data.
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  EXPECT_EQ(std::filesystem::file_size(wal_path), 4U);

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get("k").value(), "v");
}

TEST_F(StorageEngineTest, CleanCloseSyncsDatabaseBeforeResettingWal) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("k", "v").Ok());

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(engine.Close().Ok());
  }

  const auto db_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path_);
  const auto wal_truncate =
      FindCall(calls, tinydb::io::Syscall::Ftruncate, tinydb::Wal::PathFor(db_path_), db_sync + 1);
  const auto wal_sync = FindCall(calls, tinydb::io::Syscall::Fsync, tinydb::Wal::PathFor(db_path_), wal_truncate + 1);

  ASSERT_NE(db_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_truncate, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(db_sync, wal_truncate);
  EXPECT_LT(wal_truncate, wal_sync);
}

TEST_F(StorageEngineTest, CleanCloseLeavesWalIntactWhenDatabaseSyncFails) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("k", "v").Ok());
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Fsync && call.path == db_path_) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto status = engine.Close();
    EXPECT_EQ(status.Code(), tinydb::StatusCode::IoError);
    EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);
  }

  ASSERT_TRUE(engine.Close().Ok());
}

TEST_F(StorageEngineTest, LogOutgrowingItsThresholdCheckpoints) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  // Each put logs full images of every page it touches, so this comfortably
  // pushes the log past its 1 MiB checkpoint threshold at least once.
  for (int row = 0; row < 400; ++row) {
    ASSERT_TRUE(engine.Put(RowKey(row), RowValue(row, 128)).Ok());
  }

  // The workload appended well over 1 MiB of images in total, so a log
  // still at or under the threshold proves at least one reset happened.
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  EXPECT_LE(std::filesystem::file_size(wal_path), (1U << 20U));

  // And a post-checkpoint crash still recovers cleanly: the database file
  // plus the shorter log reproduce every row.
  SnapshotDatabase();
  auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();
  EXPECT_EQ(recovered.Scan("", SCAN_END).value().size(), 400U);
  EXPECT_EQ(recovered.Get(RowKey(399)).value(), RowValue(399, 128));
}
