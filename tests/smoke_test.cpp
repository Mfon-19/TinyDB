#include <gtest/gtest.h>
#include <tinydb/storage_engine.h>

TEST(StorageEngineTest, BasicPutGetRemove) {
  std::filesystem::path db_path = "test_data.db";
  auto db = tinydb::StorageEngine::Open(db_path);

  // Initially key does not exist
  EXPECT_EQ(db.Get("key1"), std::nullopt);

  // Put a value
  db.Put("key1", "value1");
  EXPECT_EQ(db.Get("key1"), "value1");

  // Overwrite a value
  db.Put("key1", "value2");
  EXPECT_EQ(db.Get("key1"), "value2");

  // Remove a value
  db.Remove("key1");
  EXPECT_EQ(db.Get("key1"), std::nullopt);

  db.Close();
}

TEST(StorageEngineTest, MoveOperations) {
  std::filesystem::path db_path = "test_data_move.db";
  auto db1 = tinydb::StorageEngine::Open(db_path);
  db1.Put("moved_key", "moved_value");

  // Move construct
  auto db2 = std::move(db1);
  EXPECT_EQ(db2.Get("moved_key"), "moved_value");
  // The moved-from db1 should be closed/empty
  EXPECT_EQ(db1.Get("moved_key"), std::nullopt);

  // Move assign
  std::filesystem::path db_path_3 = "test_data_move3.db";
  auto db3 = tinydb::StorageEngine::Open(db_path_3);
  db3 = std::move(db2);
  EXPECT_EQ(db3.Get("moved_key"), "moved_value");
  EXPECT_EQ(db2.Get("moved_key"), std::nullopt);

  db3.Close();
}

TEST(StorageEngineTest, ClosedState) {
  std::filesystem::path db_path = "test_data_closed.db";
  auto db = tinydb::StorageEngine::Open(db_path);
  db.Put("k", "v");
  db.Close();

  // Operations should not work after close
  EXPECT_EQ(db.Get("k"), std::nullopt);

  db.Put("k2", "v2");
  EXPECT_EQ(db.Get("k2"), std::nullopt);

  db.Remove("k");  // Should not crash
}
