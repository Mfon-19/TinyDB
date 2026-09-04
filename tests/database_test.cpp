#include "tinydb/database.h"
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <unistd.h>

namespace tinydb {
namespace {

class DatabaseTest : public testing::Test {
protected:
  void SetUp() override {
    directory_ = testing::TempDir() + "tinydb_test_XXXXXX";
    ASSERT_NE(mkdtemp(directory_.data()), nullptr);
    path_ = directory_ + "/database";
  }

  void TearDown() override { std::filesystem::remove_all(directory_); }

  std::string directory_;
  std::string path_;
};

TEST_F(DatabaseTest, CreatesAndReopens) {
  {
    auto opened = Database::Open(path_, 8);
    ASSERT_TRUE(opened) << opened.error().Message();
    auto missing = (*opened)->Get("key");
    ASSERT_TRUE(missing) << missing.error().Message();
    EXPECT_FALSE(missing->has_value());
    EXPECT_EQ(std::filesystem::file_size(path_), 2 * storage::PAGE_SIZE);
    auto status = (*opened)->Put("key", "value");
    ASSERT_TRUE(status.Ok()) << status.Message();
  }

  auto reopened = Database::Open(path_, 8);
  ASSERT_TRUE(reopened) << reopened.error().Message();
  auto value = (*reopened)->Get("key");
  ASSERT_TRUE(value) << value.error().Message();
  EXPECT_EQ(*value, "value");
}

TEST_F(DatabaseTest, RejectsAnotherProcess) {
  auto opened = Database::Open(path_, 8);
  ASSERT_TRUE(opened) << opened.error().Message();
  ASSERT_EXIT(
      {
        opened->reset();
        auto second = Database::Open(path_, 8);
        _exit(!second && second.error().Message() == "database is already open"
                  ? 0
                  : 1);
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(DatabaseTest, ReadsPendingWritesAndCommits) {
  auto database = Database::Open(path_, 2).value();
  ASSERT_TRUE(database->Put("a", "old").Ok());
  ASSERT_TRUE(database->Put("b", "removed").Ok());
  auto transaction = database->BeginWrite().value();
  ASSERT_TRUE(transaction->Put("a", "new").Ok());
  ASSERT_TRUE(transaction->Put("c", "inserted").Ok());
  ASSERT_TRUE(transaction->Delete("b").value());
  EXPECT_EQ(transaction->Get("a").value(), "new");
  EXPECT_EQ(transaction->Get("b").value(), std::nullopt);
  EXPECT_EQ(transaction->Get("c").value(), "inserted");
  EXPECT_EQ(database->Get("a").value(), "old");
  EXPECT_EQ(database->Get("b").value(), "removed");
  EXPECT_EQ(database->Get("c").value(), std::nullopt);

  auto cursor = transaction->Seek("").value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "a");
  ASSERT_TRUE(cursor.Next().Ok());
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "c");
  ASSERT_TRUE(cursor.Next().Ok());
  EXPECT_FALSE(cursor.Valid());

  ASSERT_TRUE(transaction->Commit().Ok());
  EXPECT_EQ(database->Get("a").value(), "new");
  EXPECT_EQ(database->Get("b").value(), std::nullopt);
  EXPECT_EQ(database->Get("c").value(), "inserted");
  EXPECT_FALSE(transaction->Get("a"));
  EXPECT_FALSE(transaction->Put("d", "late").Ok());
  EXPECT_FALSE(transaction->Commit().Ok());
  EXPECT_TRUE(database->BeginWrite().value()->Commit().Ok());
}

TEST_F(DatabaseTest, FailedWriteCannotCommit) {
  auto database = Database::Open(path_, 2).value();
  auto transaction = database->BeginWrite().value();
  ASSERT_TRUE(transaction->Put("key", "pending").Ok());
  auto cursor = transaction->Seek("").value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_FALSE(
      transaction->Put("large", std::string(storage::PAGE_SIZE, 'v')).Ok());
  EXPECT_FALSE(transaction->Get("key"));
  EXPECT_FALSE(transaction->Delete("key"));
  EXPECT_FALSE(transaction->Put("other", "value").Ok());
  EXPECT_FALSE(transaction->Seek(""));
  EXPECT_FALSE(cursor.Valid());
  EXPECT_FALSE(cursor.Next().Ok());
  EXPECT_FALSE(transaction->Commit().Ok());
  EXPECT_EQ(database->Get("key").value(), std::nullopt);
  EXPECT_TRUE(database->Put("key", "healthy").Ok());
}

TEST_F(DatabaseTest, RollbackPreservesTreeAndAllocator) {
  auto database = Database::Open(path_, 2).value();
  const auto key = [](int index) {
    return std::format("{:04}", index) + std::string(400, 'k');
  };
  const std::string value(600, 'v');
  const auto fill = [&](WriteTransaction &transaction, int count) {
    for (int index = 0; index < count; ++index) {
      ASSERT_TRUE(transaction.Put(key(index), value).Ok());
    }
  };
  const auto erase = [&](WriteTransaction &transaction) {
    for (int index = 0; index < 80; ++index) {
      ASSERT_TRUE(transaction.Delete(key(index)).value());
    }
    EXPECT_FALSE(transaction.Seek("").value().Valid());
  };
  const auto file_bytes = [&] {
    std::ifstream file(path_, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>{file}, {});
  };

  {
    auto transaction = database->BeginWrite().value();
    fill(*transaction, 80);
    ASSERT_TRUE(transaction->Commit().Ok());
  }
  const auto original = file_bytes();
  ASSERT_GT(original.size(), 2 * storage::PAGE_SIZE);
  {
    auto transaction = database->BeginWrite().value();
    erase(*transaction);
    fill(*transaction, 160);
    EXPECT_EQ(transaction->Get(key(159)).value(), value);
    EXPECT_EQ(database->Get(key(159)).value(), std::nullopt);
    for (int index = 0; index < 80; ++index) {
      EXPECT_EQ(database->Get(key(index)).value(), value);
    }
    EXPECT_EQ(file_bytes(), original);
  }
  EXPECT_EQ(file_bytes(), original);

  {
    auto transaction = database->BeginWrite().value();
    erase(*transaction);
    ASSERT_TRUE(transaction->Commit().Ok());
  }
  database.reset();
  database = Database::Open(path_, 2).value();
  const auto empty = file_bytes();
  {
    auto transaction = database->BeginWrite().value();
    fill(*transaction, 160);
    EXPECT_EQ(file_bytes(), empty);
  }
  {
    auto transaction = database->BeginWrite().value();
    fill(*transaction, 80);
    ASSERT_TRUE(transaction->Commit().Ok());
  }
  EXPECT_EQ(std::filesystem::file_size(path_), original.size());
  database.reset();
  database = Database::Open(path_, 2).value();
  auto cursor = database->Seek("").value();
  for (int index = 0; index < 80; ++index) {
    ASSERT_TRUE(cursor.Valid());
    EXPECT_EQ(cursor.Key(), key(index));
    EXPECT_EQ(cursor.Value(), value);
    ASSERT_TRUE(cursor.Next().Ok());
  }
  EXPECT_FALSE(cursor.Valid());
}

}
}
