#include <gtest/gtest.h>

#include <tinydb/database.h>

#include "io/direct_file.h"
#include "support/test_files.h"

#include <fcntl.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

auto DirectOptions() -> tinydb::Options {
  auto options = tinydb::Options{};
  options.page_io_mode = tinydb::PageIoMode::Direct;
  return options;
}

auto DirectIoUnavailableReason() -> std::optional<std::string> {
#if defined(__linux__) && defined(O_DIRECT)
  const auto path = tinydb::test::Path("direct_mode_probe");
  tinydb::test::Remove(path);
  auto reason = std::optional<std::string>{};
  {
    const auto opened = tinydb::io::DirectFile::CreateExclusive(path, O_RDWR, 0644);
    if (!opened) {
      reason = opened.error().ToString();
    }
  }
  tinydb::test::Remove(path);
  return reason;
#else
  return "this build does not support O_DIRECT";
#endif
}

void ExpectValue(tinydb::Database &database, std::string_view key, const std::string &expected) {
  const auto found = database.Get(key);
  ASSERT_TRUE(found.has_value()) << found.error().ToString();
  EXPECT_EQ(*found, std::optional<std::string>{expected});
}

}  // namespace
TEST(IoMode, DefaultUsesBufferedDatabaseTransport) {
  EXPECT_EQ(tinydb::Options{}.page_io_mode, tinydb::PageIoMode::Buffered);
  const auto legacy_positional =
      tinydb::Options{tinydb::PAGE_SIZE, tinydb::PAGE_SIZE, {}};  // NOLINT(modernize-use-designated-initializers)
  EXPECT_EQ(legacy_positional.page_io_mode, tinydb::PageIoMode::Buffered);

  const auto path = tinydb::test::Path("default_buffered_transport");
  tinydb::test::Remove(path);
  auto database_open_flags = std::vector<int>{};
  {
    const auto hook =
        tinydb::test::ScopedTestHook([&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
          if (call.syscall == tinydb::io::Syscall::Open && call.path == path) {
            database_open_flags.push_back(call.flags);
          }
          return std::nullopt;
        });
    auto opened = tinydb::Database::Open(path);
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    ASSERT_TRUE(opened->Close().Ok());
  }

  // One descriptor owns the process lock and another transports database
  // pages. The default must not silently select direct I/O for either one.
  ASSERT_GE(database_open_flags.size(), 2U);
#if defined(O_DIRECT)
  EXPECT_TRUE(std::ranges::none_of(database_open_flags, [](int flags) { return (flags & O_DIRECT) != 0; }));
#endif
  tinydb::test::Remove(path);
}

TEST(IoMode, DirectUsesDirectDatabaseTransport) {
  if (const auto reason = DirectIoUnavailableReason()) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << *reason;
  }

  const auto path = tinydb::test::Path("direct_transport");
  tinydb::test::Remove(path);
  auto database_open_flags = std::vector<int>{};
  {
    const auto hook =
        tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
          if (call.syscall == tinydb::io::Syscall::Open && call.path == path) {
            database_open_flags.push_back(call.flags);
          }
          return std::nullopt;
        }};
    auto opened = tinydb::Database::Open(path, DirectOptions());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    ASSERT_TRUE(opened->Close().Ok());
  }

  // The lock remains buffered. A separate descriptor must carry O_DIRECT for
  // database pages, which also proves that direct mode did not fall back.
  ASSERT_GE(database_open_flags.size(), 2U);
#if defined(O_DIRECT)
  EXPECT_TRUE(std::ranges::any_of(database_open_flags, [](int flags) { return (flags & O_DIRECT) != 0; }));
  EXPECT_TRUE(std::ranges::any_of(database_open_flags, [](int flags) { return (flags & O_DIRECT) == 0; }));
#endif
  tinydb::test::Remove(path);
}

TEST(IoMode, CheckpointedDataMovesBetweenBufferedAndDirect) {
  if (const auto reason = DirectIoUnavailableReason()) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << *reason;
  }

  const auto path = tinydb::test::Path("cross_mode_checkpoint");
  tinydb::test::Remove(path);
  const auto first_overflow = std::string(3U * tinydb::PAGE_SIZE + 37U, 'a');
  const auto second_overflow = std::string(2U * tinydb::PAGE_SIZE + 19U, 'b');

  {
    auto database = tinydb::Database::Open(path).value();
    ASSERT_TRUE(database.Put("inline", "from-buffered").Ok());
    ASSERT_TRUE(database.Put("overflow", first_overflow).Ok());
    ASSERT_TRUE(database.Checkpoint().Ok());
    ASSERT_TRUE(database.Close().Ok());
  }
  {
    auto opened = tinydb::Database::Open(path, DirectOptions());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto database = std::move(*opened);
    ExpectValue(database, "inline", "from-buffered");
    ExpectValue(database, "overflow", first_overflow);
    ASSERT_TRUE(database.Put("inline", "from-direct").Ok());
    ASSERT_TRUE(database.Put("overflow", second_overflow).Ok());
    ASSERT_TRUE(database.Put("direct-only", "present").Ok());
    ASSERT_TRUE(database.Checkpoint().Ok());
    ASSERT_TRUE(database.Close().Ok());
  }
  {
    auto database = tinydb::Database::Open(path).value();
    ExpectValue(database, "inline", "from-direct");
    ExpectValue(database, "overflow", second_overflow);
    ExpectValue(database, "direct-only", "present");
    ASSERT_TRUE(database.Put("inline", "buffered-again").Ok());
    ASSERT_TRUE(database.Put("overflow", first_overflow).Ok());
    ASSERT_TRUE(database.Checkpoint().Ok());
    ASSERT_TRUE(database.Close().Ok());
  }
  {
    auto opened = tinydb::Database::Open(path, DirectOptions());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto database = std::move(*opened);
    ExpectValue(database, "inline", "buffered-again");
    ExpectValue(database, "overflow", first_overflow);
    ASSERT_TRUE(database.Close().Ok());
  }
  tinydb::test::Remove(path);
}

TEST(IoMode, PendingWalRecoversAcrossModes) {
  if (const auto reason = DirectIoUnavailableReason()) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << *reason;
  }

  const auto buffered_source = tinydb::test::Path("buffered_wal_source");
  const auto direct_recovery = tinydb::test::Path("direct_wal_recovery");
  const auto direct_source = tinydb::test::Path("direct_wal_source");
  const auto buffered_recovery = tinydb::test::Path("buffered_wal_recovery");
  for (const auto &path : {buffered_source, direct_recovery, direct_source, buffered_recovery}) {
    tinydb::test::Remove(path);
  }
  const auto buffered_overflow = std::string(2U * tinydb::PAGE_SIZE + 31U, 'c');
  const auto direct_overflow = std::string(3U * tinydb::PAGE_SIZE + 47U, 'd');

  {
    auto source = tinydb::Database::Open(buffered_source).value();
    ASSERT_TRUE(source.Put("inline", "buffered-wal").Ok());
    ASSERT_TRUE(source.Put("overflow", buffered_overflow).Ok());
    tinydb::test::Copy(buffered_source, direct_recovery);

    auto opened = tinydb::Database::Open(direct_recovery, DirectOptions());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto recovered = std::move(*opened);
    ExpectValue(recovered, "inline", "buffered-wal");
    ExpectValue(recovered, "overflow", buffered_overflow);
    ASSERT_TRUE(recovered.Close().Ok());
    ASSERT_TRUE(source.Close().Ok());
  }
  {
    auto opened = tinydb::Database::Open(direct_source, DirectOptions());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto source = std::move(*opened);
    ASSERT_TRUE(source.Put("inline", "direct-wal").Ok());
    ASSERT_TRUE(source.Put("overflow", direct_overflow).Ok());
    tinydb::test::Copy(direct_source, buffered_recovery);

    auto recovered = tinydb::Database::Open(buffered_recovery).value();
    ExpectValue(recovered, "inline", "direct-wal");
    ExpectValue(recovered, "overflow", direct_overflow);
    ASSERT_TRUE(recovered.Close().Ok());
    ASSERT_TRUE(source.Close().Ok());
  }

  for (const auto &path : {buffered_source, direct_recovery, direct_source, buffered_recovery}) {
    tinydb::test::Remove(path);
  }
}
