#include <gtest/gtest.h>

#include <tinydb/database.h>

#include "support/test_files.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/*
** DATABASE GUARANTEE TESTS
**
** This is the public contract exercised through the one production API path.
** Tests deliberately avoid fixtures and storage doubles: each case owns a
** fresh path, and reopen observes the same recovery path an application uses.
** Codec and exact durability-order tests live in their focused suites.
*/

namespace {

void Cleanup(const std::filesystem::path &path) { tinydb::test::Remove(path); }

auto Fresh(std::string_view name, tinydb::Options options = {}) -> tinydb::Result<tinydb::Database> {
  const auto path = tinydb::test::Path(name);
  Cleanup(path);
  return tinydb::Database::Open(path, options);
}

}  // namespace

TEST(Database, Transactions) {
  const auto path = tinydb::test::Path("transactions");
  auto database = Fresh("transactions").value();

  {
    auto write = database.BeginWrite().value();
    ASSERT_TRUE(write.Put("doc/1", "body").Ok());
    ASSERT_TRUE(write.Put("tag/db/doc/1", "").Ok());
    EXPECT_EQ(write.Get("doc/1").value(), "body");
    EXPECT_EQ(database.Checkpoint().Code(), tinydb::StatusCode::Busy);
    ASSERT_TRUE(std::move(write).Commit().has_value());
  }
  {
    auto write = database.BeginWrite().value();
    ASSERT_TRUE(write.Put("doc/1", "discarded").Ok());
    write.Abort();
  }
  {
    auto write = database.BeginWrite().value();
    ASSERT_TRUE(write.Delete("doc/1").Ok());
  }

  EXPECT_EQ(database.Get("doc/1").value(), "body");
  EXPECT_EQ(database.Get("tag/db/doc/1").value(), "");
  Cleanup(path);
}

TEST(Database, Ranges) {
  const auto path = tinydb::test::Path("ranges");
  auto database = Fresh("ranges").value();
  for (std::size_t index = 0; index < 10; ++index) {
    ASSERT_TRUE(database.Put(tinydb::test::Key(index), tinydb::test::Value(index, 24)).Ok());
  }
  ASSERT_TRUE(database.Put(std::string(1, static_cast<char>(0x80)), "high").Ok());

  auto rows =
      tinydb::test::Rows(database, tinydb::KeyRange::Between(tinydb::test::Key(3), tinydb::test::Key(6))).value();
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows.front().first, tinydb::test::Key(3));
  EXPECT_EQ(rows.back().first, tinydb::test::Key(5));
  EXPECT_TRUE(tinydb::test::Rows(database, tinydb::KeyRange::Prefix("missing/")).value().empty());
  EXPECT_EQ(tinydb::test::Rows(database).value().back().second, "high");
  Cleanup(path);
}

TEST(Database, Snapshots) {
  const auto path = tinydb::test::Path("snapshots");
  auto database = Fresh("snapshots").value();
  ASSERT_TRUE(database.Put("key", "before").Ok());
  auto reader = std::optional<tinydb::ReadTransaction>{database.BeginRead().value()};
  auto started = std::atomic<bool>{false};
  auto finished = std::atomic<bool>{false};

  auto writer = std::thread([&] {
    auto write = database.BeginWrite().value();
    EXPECT_TRUE(write.Put("key", "after").Ok());
    started.store(true, std::memory_order_release);
    EXPECT_TRUE(std::move(write).Commit().has_value());
    finished.store(true, std::memory_order_release);
  });
  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_EQ(reader->Get("key").value(), "before");
  EXPECT_FALSE(finished.load(std::memory_order_acquire));
  reader.reset();
  writer.join();

  EXPECT_EQ(database.Get("key").value(), "after");
  EXPECT_GT(database.Stats()->maximum_publication_wait, std::chrono::nanoseconds::zero());
  Cleanup(path);
}

TEST(Database, Limits) {
  const auto path = tinydb::test::Path("limits");
  auto invalid = tinydb::Options{};
  invalid.page_cache_bytes = tinydb::PAGE_SIZE - 1U;
  const auto rejected = tinydb::Database::Open(path, invalid);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(path));

  auto options = tinydb::Options{};
  options.max_write_transaction_bytes = 2U * tinydb::PAGE_SIZE;
  auto database = tinydb::Database::Open(path, options).value();
  {
    auto write = database.BeginWrite().value();
    EXPECT_EQ(write.Put(std::string(tinydb::MAX_KEY_BYTES + 1U, 'k'), "v").Code(), tinydb::StatusCode::InvalidArgument);
    ASSERT_TRUE(write.Put("valid", "value").Ok());
    ASSERT_TRUE(std::move(write).Commit().has_value());
  }
  {
    auto write = database.BeginWrite().value();
    EXPECT_EQ(write.Put("large", std::string(2U * tinydb::PAGE_SIZE, 'v')).Code(),
              tinydb::StatusCode::ResourceExhausted);
  }
  EXPECT_EQ(database.Get("valid").value(), "value");
  Cleanup(path);
}

TEST(Database, Persistence) {
  const auto path = tinydb::test::Path("persistence");
  Cleanup(path);
  {
    auto database = tinydb::Database::Open(path).value();
    auto write = database.BeginWrite().value();
    for (std::size_t index = 0; index < 160; ++index) {
      ASSERT_TRUE(write.Put(tinydb::test::Key(index), tinydb::test::Value(index, 300)).Ok());
    }
    ASSERT_TRUE(std::move(write).Commit().has_value());
    for (std::size_t index = 0; index < 60; ++index) {
      ASSERT_TRUE(database.Delete(tinydb::test::Key(index)).Ok());
    }
    ASSERT_TRUE(database.Close().Ok());
  }
  auto reopened = tinydb::Database::Open(path).value();
  EXPECT_EQ(tinydb::test::Rows(reopened).value().size(), 100U);
  EXPECT_EQ(reopened.Get(tinydb::test::Key(159)).value(), tinydb::test::Value(159, 300));
  EXPECT_EQ(reopened.Get(tinydb::test::Key(0)).value(), std::nullopt);
  EXPECT_TRUE(reopened.Verify()->Ok());
  Cleanup(path);
}

TEST(Database, Locking) {
  const auto path = tinydb::test::Path("locking");
  auto database = Fresh("locking").value();
  auto read = std::optional<tinydb::ReadTransaction>{database.BeginRead().value()};
  auto cursor = std::optional<tinydb::Cursor>{read->Scan().value()};
  EXPECT_EQ(database.Close().Code(), tinydb::StatusCode::Busy);
  const auto second = tinydb::Database::Open(path);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().Code(), tinydb::StatusCode::Busy);
  read.reset();
  EXPECT_EQ(database.Close().Code(), tinydb::StatusCode::Busy);
  cursor.reset();
  ASSERT_TRUE(database.Close().Ok());
  EXPECT_TRUE(tinydb::Database::Open(path).has_value());
  Cleanup(path);
}

TEST(Database, LargeValues) {
  const auto path = tinydb::test::Path("large_values");
  auto database = Fresh("large_values").value();
  const auto large = std::string(3U * tinydb::PAGE_SIZE, 'x');
  ASSERT_TRUE(database.Put("large", large).Ok());
  EXPECT_EQ(database.Get("large").value(), large);
  ASSERT_TRUE(database.Put("large", "small").Ok());
  EXPECT_GT(database.Verify()->retired_pages, 0U);
  ASSERT_TRUE(database.Checkpoint().Ok());
  EXPECT_EQ(database.Verify()->retired_pages, 0U);
  EXPECT_GT(database.Verify()->reusable_pages, 0U);
  Cleanup(path);
}

TEST(Database, Model) {
  const auto path = tinydb::test::Path("model");
  Cleanup(path);
  auto expected = std::map<std::string, std::string>{};
  auto random = std::mt19937_64{0x54494e594442ULL};

  for (int epoch = 0; epoch < 4; ++epoch) {
    auto database = tinydb::Database::Open(path).value();
    for (int step = 0; step < 100; ++step) {
      const auto index = static_cast<std::size_t>(random() % 120U);
      const auto key = tinydb::test::Key(index);
      if (random() % 3U == 0U) {
        ASSERT_TRUE(database.Delete(key).Ok());
        expected.erase(key);
      } else {
        const auto value = tinydb::test::Value(index + static_cast<std::size_t>(epoch * 100 + step),
                                               1U + static_cast<std::size_t>(random() % 700U));
        ASSERT_TRUE(database.Put(key, value).Ok());
        expected[key] = value;
      }
    }
    ASSERT_TRUE(database.Checkpoint().Ok());
    const auto rows = tinydb::test::Rows(database).value();
    const auto actual = std::map<std::string, std::string>(rows.begin(), rows.end());
    EXPECT_EQ(actual, expected);
  }
  Cleanup(path);
}

TEST(Database, Health) {
  const auto path = tinydb::test::Path("health");
  auto database = Fresh("health").value();
  ASSERT_TRUE(database.Put("small", "value").Ok());
  ASSERT_TRUE(database.Put("large", std::string(2U * tinydb::PAGE_SIZE, 'v')).Ok());
  ASSERT_TRUE(database.Checkpoint().Ok());
  const auto before_database = tinydb::test::ReadFile(path);
  const auto before_wal = tinydb::test::ReadFile(tinydb::Wal::PathFor(path));

  const auto report = database.Verify();
  ASSERT_TRUE(report.has_value());
  EXPECT_TRUE(report->Ok());
  EXPECT_GT(report->pages_checked, 0U);
  EXPECT_GT(report->overflow_pages, 0U);
  const auto stats = database.Stats().value();
  EXPECT_EQ(stats.checkpoint_lsn, stats.visible_lsn);
  EXPECT_EQ(stats.dirty_pages, 0U);
  EXPECT_EQ(tinydb::test::ReadFile(path), before_database);
  EXPECT_EQ(tinydb::test::ReadFile(tinydb::Wal::PathFor(path)), before_wal);
  Cleanup(path);
}

TEST(Database, AutomaticCheckpoint) {
  auto options = tinydb::Options{};
  options.checkpoint.wal_trigger_bytes = 1;
  auto database = Fresh("automatic_checkpoint", options).value();
  const auto before = database.Stats().value();
  ASSERT_TRUE(before.checkpoint_requested);

  ASSERT_TRUE(database.Put("key", "value").Ok());
  const auto after = database.Stats().value();
  EXPECT_EQ(after.checkpoint_lsn, before.visible_lsn);
  EXPECT_GT(after.visible_lsn, after.checkpoint_lsn);
  Cleanup(tinydb::test::Path("automatic_checkpoint"));
}

TEST(Database, CrashCopy) {
  const auto path = tinydb::test::Path("crash_copy");
  const auto copy = tinydb::test::Path("crash_copy_reopen");
  auto database = Fresh("crash_copy").value();
  auto write = database.BeginWrite().value();
  for (std::size_t index = 0; index < 80; ++index) {
    ASSERT_TRUE(write.Put(tinydb::test::Key(index), tinydb::test::Value(index, 256)).Ok());
  }
  ASSERT_TRUE(std::move(write).Commit().has_value());
  tinydb::test::Copy(path, copy);

  auto recovered = tinydb::Database::Open(copy).value();
  EXPECT_EQ(tinydb::test::Rows(recovered).value().size(), 80U);
  EXPECT_EQ(recovered.Get(tinydb::test::Key(79)).value(), tinydb::test::Value(79, 256));
  Cleanup(path);
  Cleanup(copy);
}

TEST(Database, Failures) {
  const auto abort_path = tinydb::test::Path("definite_abort");
  auto database = Fresh("definite_abort").value();
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == tinydb::Wal::PathFor(abort_path)) {
          return tinydb::io::Fault{.error = EIO};
        }
        return std::nullopt;
      },
      [&] { EXPECT_EQ(database.Put("aborted", "value").Code(), tinydb::StatusCode::IoError); });
  EXPECT_EQ(database.Get("aborted").value(), std::nullopt);
  ASSERT_TRUE(database.Put("later", "value").Ok());
  Cleanup(abort_path);

  const auto limbo_path = tinydb::test::Path("indeterminate");
  auto limbo = Fresh("indeterminate").value();
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Fsync && call.path == tinydb::Wal::PathFor(limbo_path)) {
          return tinydb::io::Fault{.error = EIO};
        }
        return std::nullopt;
      },
      [&] {
        auto write = limbo.BeginWrite().value();
        EXPECT_TRUE(write.Put("doc/1", "body").Ok());
        EXPECT_TRUE(write.Put("request/save-1", "done").Ok());
        const auto committed = std::move(write).Commit();
        EXPECT_FALSE(committed.has_value());
        EXPECT_EQ(committed.error().Code(), tinydb::StatusCode::IndeterminateCommit);
      });
  EXPECT_EQ(limbo.Get("doc/1").error().Code(), tinydb::StatusCode::NeedsRecovery);
  EXPECT_TRUE(limbo.Close().Ok());

  auto reopened = tinydb::Database::Open(limbo_path).value();
  const auto document = reopened.Get("doc/1").value();
  const auto request = reopened.Get("request/save-1").value();
  EXPECT_EQ(document.has_value(), request.has_value());
  Cleanup(limbo_path);
}

TEST(Database, Concurrency) {
  const auto path = tinydb::test::Path("concurrency");
  auto database = Fresh("concurrency").value();
  ASSERT_TRUE(database.Put("shared", "value-0").Ok());
  auto stop = std::atomic<bool>{false};
  auto failures = std::atomic<std::size_t>{0};
  auto readers = std::vector<std::thread>{};
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const auto value = database.Get("shared");
        if (!value || !value->has_value() || !value->value().starts_with("value-")) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (int version = 1; version <= 40; ++version) {
    ASSERT_TRUE(database.Put("shared", "value-" + std::to_string(version)).Ok());
  }
  stop.store(true, std::memory_order_release);
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(database.Get("shared").value(), "value-40");
  Cleanup(path);
}

TEST(Database, Corruption) {
  const auto path = tinydb::test::Path("corruption");
  {
    auto database = Fresh("corruption").value();
    ASSERT_TRUE(database.Put("key", "value").Ok());
    ASSERT_TRUE(database.Close().Ok());
  }
  auto file = std::fstream(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(tinydb::FIRST_DATA_PAGE_ID * tinydb::PAGE_SIZE));
  file.put('\0');
  file.close();
  auto opened = tinydb::Database::Open(path);
  ASSERT_TRUE(opened.has_value());
  const auto value = opened->Get("key");
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().Code(), tinydb::StatusCode::Corruption);
  const auto report = opened->Verify();
  ASSERT_TRUE(report.has_value());
  EXPECT_FALSE(report->Ok());
  Cleanup(path);
}
