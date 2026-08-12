#include <gtest/gtest.h>

#include "io/direct_file.h"
#include "io/direct_io_fork_gate.h"
#include "io/testable_posix.h"
#include "support/test_files.h"

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using tinydb::StatusCode;
using tinydb::io::DirectFile;
using tinydb::io::DirectIoOperation;
using tinydb::io::DirectPageBuffer;

static_assert(!std::is_copy_constructible_v<DirectFile>);
static_assert(!std::is_copy_assignable_v<DirectFile>);
static_assert(std::is_nothrow_move_constructible_v<DirectFile>);
static_assert(std::is_nothrow_move_assignable_v<DirectFile>);
static_assert(sizeof(DirectPageBuffer) == tinydb::PAGE_SIZE);
static_assert(alignof(DirectPageBuffer) == tinydb::PAGE_SIZE);

struct CapturedAtFork final {
  std::size_t registrations{0};
  void (*prepare)(){nullptr};
  void (*parent)(){nullptr};
  void (*child)(){nullptr};
};

auto AtForkCapture() -> CapturedAtFork & {
  static auto capture = CapturedAtFork{};
  return capture;
}

auto CaptureAtFork(void (*prepare)(), void (*parent)(), void (*child)()) -> int {
  auto &capture = AtForkCapture();
  ++capture.registrations;
  capture.prepare = prepare;
  capture.parent = parent;
  capture.child = child;
  return 0;
}

void InstallCapturedAtFork() {
  static const auto installed = [] {
    tinydb::io::SetAtForkRegistrarForTest(&CaptureAtFork);
    return true;
  }();
  static_cast<void>(installed);
}

class ScopedDatabasePath final {
 public:
  explicit ScopedDatabasePath(std::string_view name) : path_(tinydb::test::Path(name)) { tinydb::test::Remove(path_); }
  ~ScopedDatabasePath() { tinydb::test::Remove(path_); }

  ScopedDatabasePath(const ScopedDatabasePath &) = delete;
  auto operator=(const ScopedDatabasePath &) -> ScopedDatabasePath & = delete;

  auto Get() const -> const std::filesystem::path & { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(DirectIoForkGate, RegistersOnceAndTracksAdmissionLifetime) {
  InstallCapturedAtFork();
  ASSERT_TRUE(tinydb::io::EnsureDirectIoForkGate().Ok());
  ASSERT_TRUE(tinydb::io::EnsureDirectIoForkGate().Ok());

  const auto &capture = AtForkCapture();
  EXPECT_EQ(capture.registrations, 1U);
  ASSERT_NE(capture.prepare, nullptr);
  ASSERT_NE(capture.parent, nullptr);
  EXPECT_EQ(capture.child, nullptr);
  EXPECT_TRUE(tinydb::io::DirectIoForkGateSnapshotForTest().registered);

  {
    auto first = DirectIoOperation::Begin();
    ASSERT_TRUE(first.has_value()) << first.error().ToString();
    auto second = DirectIoOperation::Begin();
    ASSERT_TRUE(second.has_value()) << second.error().ToString();
    EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, 2U);

    *first = std::move(*second);
    EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, 1U);
  }
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, 0U);

  capture.prepare();
  capture.parent();
  EXPECT_FALSE(tinydb::io::DirectIoForkGateSnapshotForTest().fork_pending);

  tinydb::io::SetDirectIoActiveOperationsForTest(std::numeric_limits<std::size_t>::max());
  const auto exhausted = DirectIoOperation::Begin();
  EXPECT_FALSE(exhausted.has_value());
  if (!exhausted) {
    EXPECT_EQ(exhausted.error().Code(), StatusCode::ResourceExhausted);
  }
  tinydb::io::SetDirectIoActiveOperationsForTest(0);
}

TEST(DirectFile, RejectsInvalidAccessFlagsBeforeOpening) {
  const auto path = tinydb::test::Path("direct_invalid_flags");
  auto open_calls = std::size_t{0};
  const auto hook = tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Open) {
      ++open_calls;
    }
    return std::nullopt;
  }};

  for (const auto flags : {O_WRONLY, O_RDONLY | O_CLOEXEC}) {
    const auto opened = DirectFile::OpenExisting(path, flags);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().Code(), StatusCode::InvalidArgument);
  }
  EXPECT_EQ(open_calls, 0U);
}

TEST(DirectFile, OpensWithDirectFlagsAndReportsOpenFailure) {
#if defined(__linux__) && defined(O_DIRECT) && defined(STATX_DIOALIGN) && defined(AT_EMPTY_PATH)
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_open_failure");
  auto observed_flags = 0;
  const auto hook = tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Open && call.path == file.Get()) {
      observed_flags = call.flags;
      return tinydb::io::Fault{.error = EACCES};
    }
    return std::nullopt;
  }};

  const auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().Code(), StatusCode::IoError);
  EXPECT_NE(created.error().Message().find("open"), std::string::npos);
  EXPECT_NE(created.error().Message().find(file.Get().string()), std::string::npos);
  EXPECT_NE(observed_flags & O_DIRECT, 0);
  EXPECT_NE(observed_flags & O_CLOEXEC, 0);
  EXPECT_NE(observed_flags & O_CREAT, 0);
  EXPECT_NE(observed_flags & O_EXCL, 0);
  EXPECT_EQ(observed_flags & O_ACCMODE, O_RDWR);
#else
  GTEST_SKIP() << "this build does not provide the required direct-I/O API";
#endif
}

TEST(DirectFile, ReportsStatxFailureWithoutFallingBack) {
#if defined(__linux__) && defined(O_DIRECT) && defined(STATX_DIOALIGN) && defined(AT_EMPTY_PATH)
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_statx_failure");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }

  auto statx_calls = std::size_t{0};
  const auto hook = tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Statx && call.path == file.Get()) {
      ++statx_calls;
      return tinydb::io::Fault{.error = EOPNOTSUPP};
    }
    return std::nullopt;
  }};
  const auto reopened = DirectFile::OpenExisting(file.Get(), O_RDONLY);

  ASSERT_FALSE(reopened.has_value());
  EXPECT_EQ(reopened.error().Code(), StatusCode::IoError);
  EXPECT_NE(reopened.error().Message().find("statx"), std::string::npos);
  EXPECT_NE(reopened.error().Message().find("stx_mask=unavailable"), std::string::npos);
  EXPECT_EQ(statx_calls, 1U);
#else
  GTEST_SKIP() << "this build does not provide the required direct-I/O API";
#endif
}

TEST(DirectFile, RejectsOffsetOverflowBeforeTransfer) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_offset_overflow");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }

  auto pread_calls = std::size_t{0};
  const auto hook = tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Pread) {
      ++pread_calls;
    }
    return std::nullopt;
  }};
  auto page = DirectPageBuffer{};
  const auto status = created->ReadPage(std::numeric_limits<tinydb::page_id_t>::max(), page.bytes);

  EXPECT_EQ(status.Code(), StatusCode::InvalidArgument);
  EXPECT_EQ(pread_calls, 0U);
}

TEST(DirectFile, EmptyPageReadIsCorruption) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_eof");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }

  auto page = DirectPageBuffer{};
  const auto status = created->ReadPage(0, page.bytes);
  EXPECT_EQ(status.Code(), StatusCode::Corruption);
  EXPECT_NE(status.Message().find("short read"), std::string::npos);
}

TEST(DirectFile, RetriesInterruptedWriteAndReportsReadFailure) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_transfer_faults");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }
  ASSERT_TRUE(created->EnsurePageCount(1).Ok());

  auto expected = DirectPageBuffer{};
  expected.bytes.fill(std::byte{0x5a});
  auto write_calls = std::size_t{0};
  {
    const auto hook =
        tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
          if (call.syscall == tinydb::io::Syscall::Pwrite) {
            ++write_calls;
            if (write_calls == 1) {
              return tinydb::io::Fault{.error = EINTR};
            }
          }
          return std::nullopt;
        }};
    EXPECT_TRUE(created->WritePage(0, expected.bytes).Ok());
  }
  EXPECT_EQ(write_calls, 2U);

  auto actual = DirectPageBuffer{};
  ASSERT_TRUE(created->ReadPage(0, actual.bytes).Ok());
  EXPECT_EQ(actual.bytes, expected.bytes);

  const auto hook = tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Pread) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};
  const auto failed = created->ReadPage(0, actual.bytes);
  EXPECT_EQ(failed.Code(), StatusCode::IoError);
  EXPECT_NE(failed.Message().find("pread"), std::string::npos);
}

TEST(DirectFile, EnsurePageCountOnlyGrows) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_grow_only");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }

  ASSERT_TRUE(created->EnsurePageCount(3).Ok());
  struct stat file_stat {};
  ASSERT_TRUE(created->Stat(&file_stat).Ok());
  EXPECT_EQ(file_stat.st_size, static_cast<off_t>(3U * tinydb::PAGE_SIZE));

  ASSERT_TRUE(created->EnsurePageCount(1).Ok());
  ASSERT_TRUE(created->Stat(&file_stat).Ok());
  EXPECT_EQ(file_stat.st_size, static_cast<off_t>(3U * tinydb::PAGE_SIZE));

  const auto too_large = created->EnsurePageCount(std::numeric_limits<tinydb::page_id_t>::max());
  EXPECT_EQ(too_large.Code(), StatusCode::InvalidArgument);

  const auto hook = tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Ftruncate) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};
  EXPECT_EQ(created->EnsurePageCount(4).Code(), StatusCode::IoError);
}

TEST(DirectFile, FixedSpansBypassStagingOnlyWhenRuntimeAligned) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_runtime_alignment");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }
  ASSERT_TRUE(created->EnsurePageCount(1).Ok());

  auto aligned = DirectPageBuffer{};
  aligned.bytes.fill(std::byte{0x6d});
  const void *observed = nullptr;
  {
    const auto hook =
        tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
          if (call.syscall == tinydb::io::Syscall::Pwrite) {
            observed = call.data;
          }
          return std::nullopt;
        }};
    ASSERT_TRUE(created->WritePage(0, aligned.bytes).Ok());
  }
  EXPECT_EQ(observed, aligned.bytes.data());

  auto unaligned_storage = std::array<std::byte, tinydb::PAGE_SIZE + 1U>{};
  auto unaligned = std::span<std::byte, tinydb::PAGE_SIZE>{unaligned_storage.data() + 1U, tinydb::PAGE_SIZE};
  std::ranges::fill(unaligned, std::byte{0x37});
  observed = nullptr;
  {
    const auto hook =
        tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
          if (call.syscall == tinydb::io::Syscall::Pwrite) {
            observed = call.data;
          }
          return std::nullopt;
        }};
    ASSERT_TRUE(created->WritePage(0, unaligned).Ok());
  }
  EXPECT_NE(observed, unaligned.data());
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(observed) % tinydb::PAGE_SIZE, 0U);

  std::ranges::fill(unaligned, std::byte{0});
  observed = nullptr;
  {
    const auto hook =
        tinydb::test::ScopedTestHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
          if (call.syscall == tinydb::io::Syscall::Pread) {
            observed = call.data;
          }
          return std::nullopt;
        }};
    ASSERT_TRUE(created->ReadPage(0, unaligned).Ok());
  }
  EXPECT_NE(observed, unaligned.data());
  EXPECT_TRUE(std::ranges::all_of(unaligned, [](std::byte byte) { return byte == std::byte{0x37}; }));
}

TEST(DirectFile, SynchronousBatchStagesUnalignedPagesAndRetriesShortProgress) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_sync_batch");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }
  ASSERT_TRUE(created->EnsurePageCount(3).Ok());

  auto storage = std::array<std::array<std::byte, tinydb::PAGE_SIZE + 1U>, 3>{};
  auto pages = std::array<const std::byte *, 3>{};
  for (auto index = std::size_t{0}; index < pages.size(); ++index) {
    std::ranges::fill(storage[index], std::byte{0});
    std::ranges::fill_n(storage[index].begin() + 1, tinydb::PAGE_SIZE,
                        static_cast<std::byte>(static_cast<unsigned char>(index + 1U)));
    pages[index] = storage[index].data() + 1U;
  }

  auto offsets = std::vector<std::uint64_t>{};
  auto vector_counts = std::vector<std::size_t>{};
  auto status = tinydb::Status{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::TestAction> {
        if (call.syscall != tinydb::io::Syscall::Pwritev) {
          return std::nullopt;
        }
        offsets.push_back(call.offset);
        vector_counts.push_back(call.Vectors().size());
        for (const auto &vector : call.Vectors()) {
          EXPECT_EQ(vector.iov_len, tinydb::PAGE_SIZE);
          EXPECT_EQ(reinterpret_cast<std::uintptr_t>(vector.iov_base) % tinydb::PAGE_SIZE, 0U);
        }
        if (offsets.size() == 1U) {
          return tinydb::io::Fault{.error = EINTR};
        }
        if (offsets.size() == 2U) {
          return tinydb::io::WriteLimit{.bytes = tinydb::PAGE_SIZE};
        }
        return std::nullopt;
      },
      [&] { status = created->WritePages(0, pages); });

  ASSERT_TRUE(status.Ok()) << status.ToString();
  EXPECT_EQ(offsets, (std::vector<std::uint64_t>{0, 0, tinydb::PAGE_SIZE}));
  EXPECT_EQ(vector_counts, (std::vector<std::size_t>{3, 3, 2}));
  for (auto index = std::size_t{0}; index < pages.size(); ++index) {
    auto actual = DirectPageBuffer{};
    ASSERT_TRUE(created->ReadPage(index, actual.bytes).Ok());
    EXPECT_TRUE(std::ranges::all_of(actual.bytes, [&](std::byte byte) {
      return byte == static_cast<std::byte>(static_cast<unsigned char>(index + 1U));
    }));
  }
}

TEST(DirectFile, SynchronousBatchRejectsZeroProgress) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_sync_batch_zero");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }
  ASSERT_TRUE(created->EnsurePageCount(1).Ok());

  auto page = DirectPageBuffer{};
  const auto pages = std::array<const std::byte *, 1>{page.bytes.data()};
  auto calls = std::size_t{0};
  const auto baseline_operations = tinydb::io::DirectIoForkGateSnapshotForTest().active_operations;
  auto active_during_write = std::size_t{0};
  auto status = tinydb::Status{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::TestAction> {
        if (call.syscall == tinydb::io::Syscall::Pwritev) {
          ++calls;
          active_during_write = tinydb::io::DirectIoForkGateSnapshotForTest().active_operations;
          return tinydb::io::WriteLimit{.bytes = 0};
        }
        return std::nullopt;
      },
      [&] { status = created->WritePages(0, pages); });

  EXPECT_EQ(status.Code(), StatusCode::IoError);
  EXPECT_NE(status.Message().find("no progress"), std::string::npos);
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(active_during_write, baseline_operations + 1U);
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations);
}

TEST(DirectFile, AsyncTokensValidateKernelCompletionResults) {
  InstallCapturedAtFork();
  const auto file = ScopedDatabasePath("direct_async_tokens");
  auto created = DirectFile::CreateExclusive(file.Get(), O_RDWR, 0644);
  if (!created) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << created.error().ToString();
  }

  auto pages = std::array<DirectPageBuffer, 2>{};
  const auto page_ids = std::array<tinydb::page_id_t, 2>{0, 1};
  const auto baseline_operations = tinydb::io::DirectIoForkGateSnapshotForTest().active_operations;
  auto read = created->BeginReadPages(page_ids, std::as_writable_bytes(std::span{pages}));
  ASSERT_TRUE(read.has_value()) << read.error().ToString();
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations + 1U);
  const auto completions = std::array<int, 2>{static_cast<int>(tinydb::PAGE_SIZE), static_cast<int>(tinydb::PAGE_SIZE)};
  const auto expected = std::array<std::size_t, 2>{tinydb::PAGE_SIZE, tinydb::PAGE_SIZE};
  EXPECT_TRUE(std::move(*read).Complete(completions, expected).Ok());
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations);

  {
    auto abandoned = created->BeginReadPages(page_ids, std::as_writable_bytes(std::span{pages}));
    ASSERT_TRUE(abandoned.has_value()) << abandoned.error().ToString();
    EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations + 1U);
  }
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations);

  read = created->BeginReadPages(page_ids, std::as_writable_bytes(std::span{pages}));
  ASSERT_TRUE(read.has_value()) << read.error().ToString();
  const auto failed = std::array<int, 1>{-EIO};
  const auto combined = std::array<std::size_t, 1>{2U * tinydb::PAGE_SIZE};
  EXPECT_EQ(std::move(*read).Complete(failed, combined).Code(), StatusCode::IoError);

  const auto vectors = std::array<iovec, 2>{
      iovec{.iov_base = pages[0].bytes.data(), .iov_len = tinydb::PAGE_SIZE},
      iovec{.iov_base = pages[1].bytes.data(), .iov_len = tinydb::PAGE_SIZE},
  };
  auto write = created->BeginWritePages(0, vectors);
  ASSERT_TRUE(write.has_value()) << write.error().ToString();
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations + 1U);
  EXPECT_TRUE(std::move(*write).Complete(static_cast<int>(2U * tinydb::PAGE_SIZE), 2U * tinydb::PAGE_SIZE).Ok());
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations);

  write = created->BeginWritePages(0, vectors);
  ASSERT_TRUE(write.has_value()) << write.error().ToString();
  EXPECT_EQ(std::move(*write).Complete(static_cast<int>(tinydb::PAGE_SIZE), 2U * tinydb::PAGE_SIZE).Code(),
            StatusCode::IoError);
  EXPECT_EQ(tinydb::io::DirectIoForkGateSnapshotForTest().active_operations, baseline_operations);

  tinydb::io::SetDirectIoActiveOperationsForTest(std::numeric_limits<std::size_t>::max());
  const auto exhausted = created->BeginReadPages(page_ids, std::as_writable_bytes(std::span{pages}));
  tinydb::io::SetDirectIoActiveOperationsForTest(baseline_operations);
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(exhausted.error().Code(), StatusCode::ResourceExhausted);
}

}  // namespace
