#include "tinydb/database.h"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
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

TEST_F(DatabaseTest, CreatesAndReopensDatabase) {
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
        // Drop the child's inherited descriptor; the parent remains owner.
        opened->reset();
        auto second = Database::Open(path_, 8);
        _exit(!second && second.error().Message() == "database is already open"
                  ? 0
                  : 1);
      },
      testing::ExitedWithCode(0), "");
}

} // namespace
} // namespace tinydb
