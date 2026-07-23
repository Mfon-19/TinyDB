#include <gtest/gtest.h>

#include <tinydb/database.h>

#include "storage/page.h"
#include "support/test_files.h"
#include "wal/wal_codec.h"

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

/*
** DURABILITY ORDER TESTS
**
** Crash sweeps prove state outcomes. These smaller tests pin the ordering that
** an in-process crash cannot model: synchronization must cover a log commit
** before acknowledgement, and checkpoint data must precede the superblock
** that makes it authoritative.
*/

namespace {

auto Find(const std::vector<tinydb::io::Call> &calls, tinydb::io::Syscall syscall, const std::filesystem::path &path,
          std::size_t start = 0) -> std::size_t {
  for (auto index = start; index < calls.size(); ++index) {
    if (calls[index].syscall == syscall && calls[index].path == path) {
      return index;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

auto FindDataWrite(const std::vector<tinydb::io::Call> &calls, const std::filesystem::path &path) -> std::size_t {
  for (std::size_t index = 0; index < calls.size(); ++index) {
    if (calls[index].syscall == tinydb::io::Syscall::Pwrite && calls[index].path == path &&
        calls[index].offset >= tinydb::FIRST_DATA_PAGE_ID * tinydb::PAGE_SIZE) {
      return index;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

}  // namespace

TEST(Durability, CreationOrder) {
  const auto path = tinydb::test::Path("creation_order");
  tinydb::test::Remove(path);
  auto calls = std::vector<tinydb::io::Call>{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        calls.push_back(call);
        return std::nullopt;
      },
      [&] {
        auto database = tinydb::Database::Open(path);
        EXPECT_TRUE(database.has_value());
      });

  const auto database_sync = Find(calls, tinydb::io::Syscall::Fsync, path);
  const auto directory_sync = Find(calls, tinydb::io::Syscall::Fsync, path.parent_path(), database_sync + 1U);
  EXPECT_NE(database_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_NE(directory_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(database_sync, directory_sync);
  tinydb::test::Remove(path);
}

TEST(Durability, CommitOrder) {
  const auto path = tinydb::test::Path("commit_order");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  auto calls = std::vector<tinydb::io::Call>{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        calls.push_back(call);
        return std::nullopt;
      },
      [&] { EXPECT_TRUE(database.Put("key", "value").Ok()); });

  const auto wal = tinydb::Wal::PathFor(path);
  const auto append = Find(calls, tinydb::io::Syscall::Pwrite, wal);
  const auto sync = Find(calls, tinydb::io::Syscall::Fsync, wal, append + 1U);
  EXPECT_NE(append, std::numeric_limits<std::size_t>::max());
  EXPECT_NE(sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(append, sync);
  EXPECT_EQ(FindDataWrite(calls, path), std::numeric_limits<std::size_t>::max());
  EXPECT_TRUE(database.Close().Ok());
  tinydb::test::Remove(path);
}

TEST(Durability, CheckpointOrder) {
  const auto path = tinydb::test::Path("checkpoint_order");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  ASSERT_TRUE(database.Put("key", "value").Ok());
  auto calls = std::vector<tinydb::io::Call>{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        calls.push_back(call);
        return std::nullopt;
      },
      [&] { EXPECT_TRUE(database.Checkpoint().Ok()); });

  const auto data = FindDataWrite(calls, path);
  const auto data_sync = Find(calls, tinydb::io::Syscall::Fsync, path, data + 1U);
  const auto superblock = Find(calls, tinydb::io::Syscall::Pwrite, path, data_sync + 1U);
  const auto metadata_sync = Find(calls, tinydb::io::Syscall::Fsync, path, superblock + 1U);
  const auto wal_path = tinydb::Wal::PathFor(path);
  const auto wal_reset = Find(calls, tinydb::io::Syscall::Ftruncate, wal_path, metadata_sync + 1U);
  const auto wal_header = Find(calls, tinydb::io::Syscall::Pwrite, wal_path, wal_reset + 1U);
  const auto wal_sync = Find(calls, tinydb::io::Syscall::Fsync, wal_path, wal_header + 1U);
  ASSERT_NE(data, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(data_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(superblock, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(metadata_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_reset, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_header, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(wal_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(data, data_sync);
  EXPECT_LT(data_sync, superblock);
  EXPECT_LT(superblock, metadata_sync);
  EXPECT_LT(metadata_sync, wal_reset);
  EXPECT_LT(wal_reset, wal_header);
  EXPECT_LT(wal_header, wal_sync);
  tinydb::test::Remove(path);
}

TEST(Durability, CheckpointFailure) {
  const auto path = tinydb::test::Path("checkpoint_failure");
  const auto copy = tinydb::test::Path("checkpoint_failure_copy");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  ASSERT_TRUE(database.Put("key", "value").Ok());
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Fsync && call.path == path) {
          return tinydb::io::Fault{.error = EIO};
        }
        return std::nullopt;
      },
      [&] { EXPECT_EQ(database.Checkpoint().Code(), tinydb::StatusCode::IoError); });
  tinydb::test::Copy(path, copy);
  auto recovered = tinydb::Database::Open(copy).value();
  EXPECT_EQ(recovered.Get("key").value(), "value");
  tinydb::test::Remove(path);
  tinydb::test::Remove(copy);
}

TEST(Durability, WalResetFailure) {
  const auto path = tinydb::test::Path("wal_reset_failure");
  const auto copy = tinydb::test::Path("wal_reset_failure_copy");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  ASSERT_TRUE(database.Put("key", "value").Ok());
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == tinydb::Wal::PathFor(path) &&
            call.offset == 0) {
          return tinydb::io::Fault{.error = EIO};
        }
        return std::nullopt;
      },
      [&] { EXPECT_EQ(database.Checkpoint().Code(), tinydb::StatusCode::NeedsRecovery); });

  // The database superblock was durable before reset began, so even an empty
  // replacement WAL is a complete base, not data loss.
  tinydb::test::Copy(path, copy);
  auto recovered = tinydb::Database::Open(copy).value();
  EXPECT_EQ(recovered.Get("key").value(), "value");
  tinydb::test::Remove(path);
  tinydb::test::Remove(copy);
}

TEST(Durability, TornTail) {
  const auto path = tinydb::test::Path("torn_tail");
  const auto copy = tinydb::test::Path("torn_tail_copy");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  ASSERT_TRUE(database.Put("key", "value").Ok());
  tinydb::test::Copy(path, copy);
  {
    auto tail = std::ofstream(tinydb::Wal::PathFor(copy), std::ios::binary | std::ios::app);
    tail.write("torn", 4);
  }
  auto recovered = tinydb::Database::Open(copy).value();
  EXPECT_EQ(recovered.Get("key").value(), "value");
  tinydb::test::Remove(path);
  tinydb::test::Remove(copy);
}

TEST(Durability, MiddleCorruption) {
  const auto path = tinydb::test::Path("middle_corruption");
  const auto copy = tinydb::test::Path("middle_corruption_copy");
  const auto framing_copy = tinydb::test::Path("middle_framing_corruption_copy");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  ASSERT_TRUE(database.Put("key", "value").Ok());
  tinydb::test::Copy(path, copy);
  tinydb::test::Copy(path, framing_copy);
  const auto before = tinydb::test::ReadFile(copy);
  {
    auto wal = std::fstream(tinydb::Wal::PathFor(copy), std::ios::binary | std::ios::in | std::ios::out);
    const auto offset = static_cast<std::streamoff>(tinydb::wal_format::HEADER_BYTES + 16U);
    wal.seekg(offset);
    char byte = 0;
    wal.get(byte);
    wal.seekp(offset);
    wal.put(static_cast<char>(byte ^ 1));
  }
  const auto opened = tinydb::Database::Open(copy);
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(tinydb::test::ReadFile(copy), before);

  // Damage to the first record's framing must not disguise the complete state
  // record after it as an ignorable torn tail.
  {
    auto wal = std::fstream(tinydb::Wal::PathFor(framing_copy), std::ios::binary | std::ios::in | std::ios::out);
    wal.seekg(static_cast<std::streamoff>(tinydb::wal_format::HEADER_BYTES));
    char byte = 0;
    wal.get(byte);
    wal.seekp(static_cast<std::streamoff>(tinydb::wal_format::HEADER_BYTES));
    wal.put(static_cast<char>(byte ^ 1));
  }
  const auto framing_opened = tinydb::Database::Open(framing_copy);
  ASSERT_FALSE(framing_opened.has_value());
  EXPECT_EQ(framing_opened.error().Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(tinydb::test::ReadFile(framing_copy), before);

  tinydb::test::Remove(path);
  tinydb::test::Remove(copy);
  tinydb::test::Remove(framing_copy);
}

TEST(Durability, CheckpointResetsWal) {
  const auto path = tinydb::test::Path("wal_reset");
  const auto copy = tinydb::test::Path("wal_reset_copy");
  tinydb::test::Remove(path);
  auto options = tinydb::Options{};
  options.checkpoint.wal_trigger_bytes = 64U << 20U;
  options.checkpoint.hard_wal_bytes = 128U << 20U;
  auto database = tinydb::Database::Open(path, options).value();
  for (std::size_t index = 0; index < 8; ++index) {
    ASSERT_TRUE(database.Put(tinydb::test::Key(index), tinydb::test::Value(index, 600)).Ok());
  }
  EXPECT_GT(database.Stats()->wal_bytes, tinydb::wal_format::HEADER_BYTES);
  ASSERT_TRUE(database.Checkpoint().Ok());
  EXPECT_EQ(database.Stats()->wal_bytes, tinydb::wal_format::HEADER_BYTES);
  tinydb::test::Copy(path, copy);
  auto recovered = tinydb::Database::Open(copy, options).value();
  EXPECT_EQ(tinydb::test::Rows(recovered).value().size(), 8U);
  tinydb::test::Remove(path);
  tinydb::test::Remove(copy);
}

TEST(Durability, ForeignWal) {
  const auto first = tinydb::test::Path("foreign_first");
  const auto second = tinydb::test::Path("foreign_second");
  tinydb::test::Remove(first);
  tinydb::test::Remove(second);
  {
    auto database = tinydb::Database::Open(first).value();
    ASSERT_TRUE(database.Put("key", "first").Ok());
  }
  {
    auto database = tinydb::Database::Open(second).value();
    ASSERT_TRUE(database.Put("key", "second").Ok());
  }
  std::filesystem::copy_file(tinydb::Wal::PathFor(second), tinydb::Wal::PathFor(first),
                             std::filesystem::copy_options::overwrite_existing);
  const auto opened = tinydb::Database::Open(first);
  ASSERT_FALSE(opened.has_value());
  EXPECT_TRUE(opened.error().Code() == tinydb::StatusCode::InvalidArgument ||
              opened.error().Code() == tinydb::StatusCode::Corruption);
  tinydb::test::Remove(first);
  tinydb::test::Remove(second);
}

TEST(Durability, RecoveryRetry) {
  const auto path = tinydb::test::Path("recovery_retry");
  const auto copy = tinydb::test::Path("recovery_retry_copy");
  tinydb::test::Remove(path);
  auto database = tinydb::Database::Open(path).value();
  ASSERT_TRUE(database.Put("key", "value").Ok());
  tinydb::test::Copy(path, copy);
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Fsync && call.path == copy) {
          return tinydb::io::Fault{.error = EIO};
        }
        return std::nullopt;
      },
      [&] {
        const auto opened = tinydb::Database::Open(copy);
        EXPECT_FALSE(opened.has_value());
      });
  auto recovered = tinydb::Database::Open(copy).value();
  EXPECT_EQ(recovered.Get("key").value(), "value");
  tinydb::test::Remove(path);
  tinydb::test::Remove(copy);
}
