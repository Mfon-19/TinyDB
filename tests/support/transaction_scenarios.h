#pragma once

#include <gtest/gtest.h>
#include <tinydb/status.h>

#include <optional>
#include <string>

namespace tinydb::test_support {

// These scenarios use only the semantic adapter surface shared by the
// reference model and the TinyDB test adapter. They intentionally do
// not know about pages, the cache, WAL records, or the current Database.

template <typename Database>
void CommitPublishesAtomically(Database &database) {
  auto transaction = database.BeginWrite();
  ASSERT_TRUE(transaction.has_value());

  ASSERT_EQ(transaction->Put("doc/1", "new contents"), StatusCode::Ok);
  ASSERT_EQ(transaction->Put("tag/database/doc/1", ""), StatusCode::Ok);

  EXPECT_EQ(transaction->Get("doc/1"), std::optional<std::string>{"new contents"});
  EXPECT_EQ(database.Get("doc/1"), std::nullopt);
  EXPECT_EQ(database.Get("tag/database/doc/1"), std::nullopt);

  ASSERT_TRUE(transaction->Commit());
  EXPECT_EQ(database.Get("doc/1"), std::optional<std::string>{"new contents"});
  EXPECT_EQ(database.Get("tag/database/doc/1"), std::optional<std::string>{""});
}

template <typename Database>
void AbortDiscardsAllChanges(Database &database) {
  auto transaction = database.BeginWrite();
  ASSERT_TRUE(transaction.has_value());
  ASSERT_EQ(transaction->Put("key", "aborted"), StatusCode::Ok);
  transaction->Abort();
  EXPECT_EQ(database.Get("key"), std::nullopt);
}

template <typename Database>
void DestructionAborts(Database &database) {
  {
    auto transaction = database.BeginWrite();
    ASSERT_TRUE(transaction.has_value());
    ASSERT_EQ(transaction->Put("key", "also aborted"), StatusCode::Ok);
  }
  EXPECT_EQ(database.Get("key"), std::nullopt);
  EXPECT_TRUE(database.BeginWrite().has_value());
}

template <typename Database>
void OverwriteDeleteAndReadOwnWrites(Database &database) {
  auto first = database.BeginWrite();
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->Put("key", "first"), StatusCode::Ok);
  ASSERT_TRUE(first->Commit());

  auto second = database.BeginWrite();
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(second->Put("key", "second"), StatusCode::Ok);
  EXPECT_EQ(second->Delete("absent"), StatusCode::Ok);
  EXPECT_EQ(second->Get("key"), std::optional<std::string>{"second"});
  EXPECT_EQ(second->Delete("key"), StatusCode::Ok);
  EXPECT_EQ(second->Get("key"), std::nullopt);
  ASSERT_TRUE(second->Commit());
  EXPECT_EQ(database.Get("key"), std::nullopt);
}

template <typename Database>
void ScanUsesHalfOpenOptionalBounds(Database &database) {
  auto transaction = database.BeginWrite();
  ASSERT_TRUE(transaction.has_value());
  for (const auto *key : {"a", "b", "c", "d"}) {
    ASSERT_EQ(transaction->Put(key, key), StatusCode::Ok);
  }
  ASSERT_TRUE(transaction->Commit());

  using Rows = typename Database::Rows;
  EXPECT_EQ(database.Scan("b", "d"), (Rows{{"b", "b"}, {"c", "c"}}));
  EXPECT_EQ(database.Scan(std::nullopt, "c"), (Rows{{"a", "a"}, {"b", "b"}}));
  EXPECT_EQ(database.Scan("c", std::nullopt), (Rows{{"c", "c"}, {"d", "d"}}));
  EXPECT_TRUE(database.Scan("c", "c").empty());
  EXPECT_TRUE(database.Scan("d", "b").empty());
}

template <typename Database>
void KeysUseUnsignedByteOrder(Database &database) {
  auto transaction = database.BeginWrite();
  ASSERT_TRUE(transaction.has_value());
  ASSERT_EQ(transaction->Put(std::string{"\x80", 1}, "high"), StatusCode::Ok);
  ASSERT_EQ(transaction->Put(std::string{"\x7f", 1}, "low"), StatusCode::Ok);
  ASSERT_TRUE(transaction->Commit());

  const auto rows = database.Scan();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].first, (std::string{"\x7f", 1}));
  EXPECT_EQ(rows[1].first, (std::string{"\x80", 1}));
}

template <typename Database>
void OnlyOneWriterIsAdmitted(Database &database) {
  auto first = database.BeginWrite();
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(database.BeginWrite().has_value());
  first->Abort();
  EXPECT_TRUE(database.BeginWrite().has_value());
}

}  // namespace tinydb::test_support
