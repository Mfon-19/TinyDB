#include "TinyDB/database.h"

#include <filesystem>

#include <gtest/gtest.h>

TEST(DatabaseSmokeTest, OpensNonEmptyPath) {
  auto database = TinyDB::Database::open("test.db");

  ASSERT_TRUE(database.is_ok()) << database.status().message();
  EXPECT_EQ(database.value().path().filename(), std::filesystem::path("test.db"));
}

TEST(DatabaseSmokeTest, RejectsEmptyPath) {
  const auto database = TinyDB::Database::open("");

  ASSERT_FALSE(database.is_ok());
  EXPECT_EQ(database.status().code(), TinyDB::StatusCode::kInvalidArgument);
}
