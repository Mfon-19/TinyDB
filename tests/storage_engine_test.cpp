#include <gtest/gtest.h>
#include <tinydb/storage_engine.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
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

class StorageEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto stem = "tinydb_engine_" + std::string(info->name()) + "_" +
                      std::to_string(::getpid());
    db_path_ = std::filesystem::temp_directory_path() / (stem + ".db");
    second_db_path_ = std::filesystem::temp_directory_path() / (stem + "_b.db");
    std::filesystem::remove(db_path_);
    std::filesystem::remove(second_db_path_);
  }

  void TearDown() override {
    std::filesystem::remove(db_path_);
    std::filesystem::remove(second_db_path_);
  }

  std::filesystem::path db_path_;
  std::filesystem::path second_db_path_;
};

TEST_F(StorageEngineTest, OpenCreatesEmptyDatabase) {
  auto engine = tinydb::StorageEngine::Open(db_path_);

  EXPECT_TRUE(std::filesystem::exists(db_path_));
  EXPECT_EQ(engine.Get("anything"), std::nullopt);
  EXPECT_TRUE(engine.Scan("", SCAN_END).empty());
  engine.Remove("anything");  // removing from an empty database is a no-op
}

TEST_F(StorageEngineTest, PutGetRemoveRoundTrip) {
  auto engine = tinydb::StorageEngine::Open(db_path_);

  EXPECT_EQ(engine.Put("apple", "red"), tinydb::PutStatus::Ok);
  EXPECT_EQ(engine.Put("banana", "yellow"), tinydb::PutStatus::Ok);

  EXPECT_EQ(engine.Get("apple"), std::optional<std::string>{"red"});
  EXPECT_EQ(engine.Get("banana"), std::optional<std::string>{"yellow"});

  engine.Remove("apple");
  EXPECT_EQ(engine.Get("apple"), std::nullopt);
  EXPECT_EQ(engine.Get("banana"), std::optional<std::string>{"yellow"});
}

TEST_F(StorageEngineTest, PutReplacesExistingValue) {
  auto engine = tinydb::StorageEngine::Open(db_path_);

  EXPECT_EQ(engine.Put("key", "first"), tinydb::PutStatus::Ok);
  EXPECT_EQ(engine.Put("key", "second"), tinydb::PutStatus::Ok);

  EXPECT_EQ(engine.Get("key"), std::optional<std::string>{"second"});
  EXPECT_EQ(engine.Scan("", SCAN_END).size(), 1);
}

TEST_F(StorageEngineTest, EntrySizeCapIsEnforced) {
  auto engine = tinydb::StorageEngine::Open(db_path_);

  // Exactly at the cap is accepted.
  const auto key = std::string{"key"};
  const auto max_value = RowValue(0, tinydb::MAX_ENTRY_BYTES - key.size());
  EXPECT_EQ(engine.Put(key, max_value), tinydb::PutStatus::Ok);

  // One byte over is rejected and must not disturb existing data.
  const auto oversized = max_value + "x";
  EXPECT_EQ(engine.Put("other", oversized), tinydb::PutStatus::EntryTooLarge);
  EXPECT_EQ(engine.Put(key, oversized), tinydb::PutStatus::EntryTooLarge);

  EXPECT_EQ(engine.Get("other"), std::nullopt);
  EXPECT_EQ(engine.Get(key), std::optional<std::string>{max_value});
}

TEST_F(StorageEngineTest, ScanBoundsAreHalfOpen) {
  auto engine = tinydb::StorageEngine::Open(db_path_);
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(engine.Put(RowKey(i), RowValue(i, 20)), tinydb::PutStatus::Ok);
  }

  const auto rows = engine.Scan(RowKey(3), RowKey(6));
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows.front().first, RowKey(3));
  EXPECT_EQ(rows.back().first, RowKey(5));

  EXPECT_TRUE(engine.Scan(RowKey(4), RowKey(4)).empty());
}

TEST_F(StorageEngineTest, ClosedHandleRefusesWork) {
  auto engine = tinydb::StorageEngine::Open(db_path_);
  ASSERT_EQ(engine.Put("key", "value"), tinydb::PutStatus::Ok);

  engine.Close();

  EXPECT_EQ(engine.Put("key", "other"), tinydb::PutStatus::Closed);
  EXPECT_EQ(engine.Get("key"), std::nullopt);
  EXPECT_TRUE(engine.Scan("", SCAN_END).empty());
  engine.Remove("key");  // ignored, must not crash
  engine.Close();        // closing twice is safe

  // The pre-close write reached the file.
  auto reopened = tinydb::StorageEngine::Open(db_path_);
  EXPECT_EQ(reopened.Get("key"), std::optional<std::string>{"value"});
}

TEST_F(StorageEngineTest, DataSurvivesExplicitClose) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_);
    ASSERT_EQ(engine.Put("persist", "me"), tinydb::PutStatus::Ok);
    engine.Close();
  }

  auto engine = tinydb::StorageEngine::Open(db_path_);
  EXPECT_EQ(engine.Get("persist"), std::optional<std::string>{"me"});
}

TEST_F(StorageEngineTest, DataSurvivesDestructor) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_);
    ASSERT_EQ(engine.Put("persist", "me too"), tinydb::PutStatus::Ok);
    // No Close(): the destructor must flush.
  }

  auto engine = tinydb::StorageEngine::Open(db_path_);
  EXPECT_EQ(engine.Get("persist"), std::optional<std::string>{"me too"});
}

TEST_F(StorageEngineTest, ReopenedDatabaseAcceptsMutations) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_);
    for (int i = 0; i < 20; ++i) {
      ASSERT_EQ(engine.Put(RowKey(i), RowValue(i, 30)), tinydb::PutStatus::Ok);
    }
  }
  {
    auto engine = tinydb::StorageEngine::Open(db_path_);
    engine.Remove(RowKey(5));
    ASSERT_EQ(engine.Put(RowKey(100), RowValue(100, 30)),
              tinydb::PutStatus::Ok);
  }

  auto engine = tinydb::StorageEngine::Open(db_path_);
  EXPECT_EQ(engine.Get(RowKey(5)), std::nullopt);
  EXPECT_EQ(engine.Get(RowKey(100)),
            std::optional<std::string>{RowValue(100, 30)});
  EXPECT_EQ(engine.Scan("", SCAN_END).size(), 20);  // 20 - 1 + 1
}

TEST_F(StorageEngineTest, LargeWorkloadSurvivesReopen) {
  auto model = std::map<std::string, std::string>{};

  {
    auto engine = tinydb::StorageEngine::Open(db_path_);
    // Enough data for many leaf splits and at least one root split.
    for (int i = 0; i < 400; ++i) {
      const auto key = RowKey(i);
      const auto value = RowValue(i, 50 + (static_cast<std::size_t>(i) * 13) % 400);
      ASSERT_EQ(engine.Put(key, value), tinydb::PutStatus::Ok);
      model[key] = value;
    }
    for (int i = 0; i < 400; i += 3) {
      engine.Remove(RowKey(i));
      model.erase(RowKey(i));
    }
  }

  auto engine = tinydb::StorageEngine::Open(db_path_);
  const auto rows = engine.Scan("", SCAN_END);
  ASSERT_EQ(rows.size(), model.size());
  auto it = model.begin();
  for (const auto &[key, value] : rows) {
    EXPECT_EQ(key, it->first);
    EXPECT_EQ(value, it->second);
    ++it;
  }
}

TEST_F(StorageEngineTest, MoveTransfersOwnership) {
  auto first = tinydb::StorageEngine::Open(db_path_);
  ASSERT_EQ(first.Put("moved", "data"), tinydb::PutStatus::Ok);

  auto second = std::move(first);

  // The moved-from handle acts closed.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(first.Put("x", "y"), tinydb::PutStatus::Closed);

  // The destination owns the database.
  EXPECT_EQ(second.Get("moved"), std::optional<std::string>{"data"});
  EXPECT_EQ(second.Put("more", "rows"), tinydb::PutStatus::Ok);
}

TEST_F(StorageEngineTest, MoveAssignClosesTheOldDatabase) {
  auto target = tinydb::StorageEngine::Open(db_path_);
  ASSERT_EQ(target.Put("old", "database"), tinydb::PutStatus::Ok);

  auto source = tinydb::StorageEngine::Open(second_db_path_);
  ASSERT_EQ(source.Put("new", "database"), tinydb::PutStatus::Ok);

  target = std::move(source);

  // target now serves the second database...
  EXPECT_EQ(target.Get("new"), std::optional<std::string>{"database"});
  EXPECT_EQ(target.Get("old"), std::nullopt);

  // ...and the first database was flushed when target closed it.
  auto reopened = tinydb::StorageEngine::Open(db_path_);
  EXPECT_EQ(reopened.Get("old"), std::optional<std::string>{"database"});
}

TEST_F(StorageEngineTest, OpenRejectsForeignFiles) {
  {
    auto file = std::ofstream{db_path_};
    file << "this is not a tinydb database, just some text\n";
  }

  EXPECT_THROW(static_cast<void>(tinydb::StorageEngine::Open(db_path_)),
               std::runtime_error);
}

}  // namespace
