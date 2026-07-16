#include <gtest/gtest.h>

#include <tinydb/database.h>
#include "wal/wal.h"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

/*
** PUBLIC CURSOR CONTRACT TESTS
**
** These tests use only application-facing transaction and cursor objects. A
** cursor owns one shared snapshot admission and a bounded range. It may outlive
** its ReadTransaction wrapper, but that lifetime must keep the database core
** open and delay a writer's atomic publication until the cursor is released.
**
** Keys are compared as unsigned bytes. Prefix ranges therefore include the
** difficult all-0xff case, whose interval has no finite upper bound.
*/

using Row = std::pair<std::string, std::string>;
using Rows = std::vector<Row>;

auto TestPath(std::string_view name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tinydb_cursor_" + std::string(name) + "_" + std::to_string(::getpid()) + ".db");
}

void RemoveDatabase(const std::filesystem::path &path) {
  std::filesystem::remove(path);
  const auto wal = tinydb::Wal::PathFor(path);
  std::filesystem::remove(wal);
  auto parent = wal.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const auto prefix = wal.filename().string() + ".";
  for (const auto &entry : std::filesystem::directory_iterator(parent)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with(prefix) && name.ends_with(".segment")) {
      std::filesystem::remove(entry.path());
    }
  }
}

auto ReadRows(tinydb::Cursor &cursor) -> Rows {
  auto rows = Rows{};
  while (cursor.Valid()) {
    auto value = cursor.CopyValue();
    EXPECT_TRUE(value.has_value());
    if (!value) {
      break;
    }
    rows.emplace_back(cursor.Key(), std::move(*value));
    EXPECT_TRUE(cursor.Next().Ok());
  }
  return rows;
}

struct Fixture final {
  explicit Fixture(std::string_view name) : path(TestPath(name)) {
    RemoveDatabase(path);
    database.emplace(tinydb::Database::Open(path).value());
  }

  ~Fixture() {
    database.reset();
    RemoveDatabase(path);
  }

  void Put(std::string key, std::string value = {}) {
    if (value.empty()) {
      value = key;
    }
    ASSERT_TRUE(database->Put(key, value).Ok());
  }

  std::filesystem::path path;
  std::optional<tinydb::Database> database;
};

}  // namespace

TEST(CursorTest, OptionalBoundsAndPrefixesUseUnsignedHalfOpenOrder) {
  auto fixture = Fixture("ranges");
  for (const auto &key : std::vector<std::string>{"", "a", "ab", std::string{"ab\xff", 3}, "ac", std::string{"\xff", 1},
                                                  std::string{"\xff\xff", 2}}) {
    fixture.Put(key, "v:" + key);
  }

  const auto scan = [&](tinydb::KeyRange range) {
    auto transaction = fixture.database->BeginRead().value();
    auto cursor = transaction.Scan(std::move(range)).value();
    return ReadRows(cursor);
  };

  EXPECT_EQ(scan(tinydb::KeyRange::Between("a", "ac")),
            (Rows{{"a", "v:a"}, {"ab", "v:ab"}, {std::string{"ab\xff", 3}, std::string{"v:ab\xff", 5}}}));
  EXPECT_EQ(scan(tinydb::KeyRange::Until("ab")), (Rows{{"", "v:"}, {"a", "v:a"}}));
  EXPECT_EQ(scan(tinydb::KeyRange::From("ac")).size(), 3U);
  EXPECT_TRUE(scan(tinydb::KeyRange::Between("ac", "a")).empty());
  EXPECT_EQ(scan(tinydb::KeyRange::Prefix("ab")).size(), 2U);
  EXPECT_EQ(scan(tinydb::KeyRange::Prefix(std::string{"\xff", 1})).size(), 2U);
  EXPECT_EQ(scan(tinydb::KeyRange::Prefix("")).size(), 7U);
}

TEST(CursorTest, FirstSeekNextAndValueOwnershipRespectTheRange) {
  auto fixture = Fixture("movement");
  for (const auto *key : {"a", "b", "c", "d", "e"}) {
    fixture.Put(key, std::string{"value-"} + key);
  }

  auto transaction = fixture.database->BeginRead().value();
  auto cursor = transaction.Scan(tinydb::KeyRange::Between("b", "e")).value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "b");
  EXPECT_EQ(cursor.ValueSize(), 7U);
  EXPECT_EQ(cursor.CopyValue().value(), "value-b");

  // Seeking below the range clamps to its inclusive lower bound.
  ASSERT_TRUE(cursor.Seek("a").Ok());
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "b");
  const auto borrowed_key = cursor.Key();
  EXPECT_EQ(cursor.CopyValue().value(), "value-b");
  EXPECT_EQ(borrowed_key, "b");  // no movement: the key view is still live

  ASSERT_TRUE(cursor.Seek("d").Ok());
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "d");
  ASSERT_TRUE(cursor.Next().Ok());
  EXPECT_FALSE(cursor.Valid());  // "e" is the exclusive upper bound
  EXPECT_EQ(cursor.Next().Code(), tinydb::StatusCode::InvalidArgument);

  ASSERT_TRUE(cursor.First().Ok());
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "b");
}

TEST(CursorTest, CursorOutlivesTransactionAndDelaysWriterPublication) {
  auto fixture = Fixture("snapshot_lifetime");
  fixture.Put("key", "old");

  auto cursor = std::optional<tinydb::Cursor>{};
  {
    auto transaction = fixture.database->BeginRead().value();
    cursor.emplace(transaction.Scan().value());
  }
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->CopyValue().value(), "old");
  EXPECT_EQ(fixture.database->Close().Code(), tinydb::StatusCode::Busy);

  auto writer = fixture.database->BeginWrite().value();
  ASSERT_TRUE(writer.Put("key", "new").Ok());
  const auto wal_path = tinydb::Wal::PathFor(fixture.path);
  const auto wal_size_before = std::filesystem::file_size(wal_path);
  auto entered_commit = std::atomic<bool>{false};
  auto commit_finished = std::atomic<bool>{false};
  auto committed = std::optional<tinydb::Result<tinydb::CommitInfo>>{};
  auto thread = std::thread([&] {
    entered_commit.store(true, std::memory_order_release);
    committed.emplace(std::move(writer).Commit());
    commit_finished.store(true, std::memory_order_release);
  });
  while (!entered_commit.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  auto durable_append_observed = false;
  for (auto attempt = 0; attempt < 100'000; ++attempt) {
    if (std::filesystem::file_size(wal_path) > wal_size_before) {
      durable_append_observed = true;
      break;
    }
    std::this_thread::yield();
  }
  EXPECT_TRUE(durable_append_observed);
  EXPECT_FALSE(commit_finished.load(std::memory_order_acquire));
  EXPECT_EQ(cursor->CopyValue().value(), "old");

  cursor.reset();
  thread.join();
  ASSERT_TRUE(committed.has_value());
  EXPECT_TRUE(committed->has_value());
  EXPECT_EQ(fixture.database->Get("key").value(), "new");
}

TEST(CursorTest, CheckpointAndLeafSplitsDoNotInvalidateTraversal) {
  auto fixture = Fixture("splits_checkpoint");
  for (auto row = 0; row < 500; ++row) {
    auto key = std::string("key-") + (row < 100 ? "0" : "") + (row < 10 ? "0" : "") + std::to_string(row);
    fixture.Put(std::move(key), std::string(300, static_cast<char>('a' + row % 26)));
  }

  auto transaction = fixture.database->BeginRead().value();
  auto cursor = transaction.Scan().value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "key-000");
  ASSERT_TRUE(fixture.database->Checkpoint().Ok());

  auto count = std::size_t{0};
  while (cursor.Valid()) {
    EXPECT_EQ(cursor.ValueSize(), 300U);
    ++count;
    ASSERT_TRUE(cursor.Next().Ok());
  }
  EXPECT_EQ(count, 500U);
}
