#include <gtest/gtest.h>

#include "cache/direct_io_engine.h"
#include "cache/page_arena.h"
#include "io/testable_posix.h"
#include "storage/disk_manager.h"
#include "support/test_files.h"

#include <tinydb/options.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {

auto WriteCheckpointPage(tinydb::DiskManager &disk, tinydb::page_id_t page_id, const char *data,
                         tinydb::page_id_t captured_logical_page_count) -> tinydb::Status {
  const auto pages = std::array{reinterpret_cast<const std::byte *>(data)};
  return disk.WriteCheckpointPages(page_id, pages, captured_logical_page_count);
}

class ScopedDatabase final {
 public:
  explicit ScopedDatabase(const char *name) : path_(tinydb::test::Path(name)) { tinydb::test::Remove(path_); }
  ~ScopedDatabase() { tinydb::test::Remove(path_); }

  auto Path() const -> const std::filesystem::path & { return path_; }

 private:
  std::filesystem::path path_;
};

struct BlockingCompletion final {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<tinydb::page_id_t, 8> first_page_ids{};
  std::array<std::size_t, 8> page_counts{};
  std::size_t calls{0};
  bool entered{false};
  bool released{false};
};

void BlockCompletion(void *context, bool writing, tinydb::page_id_t first_page_id, std::size_t page_count,
                     int *completion_result) noexcept {  // NOLINT(readability-non-const-parameter)
  static_cast<void>(writing);
  static_cast<void>(completion_result);
  auto &completion = *static_cast<BlockingCompletion *>(context);
  auto lock = std::unique_lock(completion.mutex);
  if (completion.calls < completion.first_page_ids.size()) {
    completion.first_page_ids[completion.calls] = first_page_id;
    completion.page_counts[completion.calls] = page_count;
  }
  ++completion.calls;
  completion.entered = true;
  completion.changed.notify_all();
  completion.changed.wait(lock, [&completion] { return completion.released; });
}

void ReleaseCompletion(BlockingCompletion *completion) {
  auto lock = std::lock_guard(completion->mutex);
  completion->released = true;
  completion->changed.notify_all();
}

TEST(DirectIoEngine, BufferedTransportHasNoNativeBackend) {
  const auto database_file = ScopedDatabase("direct_engine_buffered");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Buffered);
  ASSERT_TRUE(disk.has_value()) << disk.error().ToString();

  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  EXPECT_FALSE(engine.Available());

  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(1);
  ASSERT_TRUE(arena->AcquireBatch(pages));
  EXPECT_EQ(engine.ScheduleExact({tinydb::FIRST_DATA_PAGE_ID}, std::move(pages)), nullptr);
}

TEST(DirectIoEngine, PosixFaultHookDisablesNativeEngine) {
  const auto database_file = ScopedDatabase("direct_engine_fault_hook");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(1);
  ASSERT_TRUE(arena->AcquireBatch(pages));

  const auto hook = tinydb::test::ScopedTestHook{
      [](const tinydb::io::Call &) -> std::optional<tinydb::io::Fault> { return std::nullopt; }};
  EXPECT_FALSE(engine.Available());
  EXPECT_EQ(engine.ScheduleExact({tinydb::FIRST_DATA_PAGE_ID}, std::move(pages)), nullptr);
}

TEST(DirectIoEngine, ReadsAndWritesAlignedDirectArenaPages) {
  const auto database_file = ScopedDatabase("direct_engine_transfers");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  constexpr auto logical_page_count = tinydb::page_id_t{6};
  ASSERT_TRUE(disk->EnsurePageCount(logical_page_count).Ok());
  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, logical_page_count, 0).Ok());

  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  if (!engine.Available()) {
    GTEST_SKIP() << "native io_uring is unavailable; synchronous direct fallback remains active";
  }

  auto arena = tinydb::cache::PageArena::CreateDirect(4);
  auto leases = std::array<tinydb::cache::PageArena::Lease, 3>{};
  ASSERT_TRUE(arena->AcquireBatch(leases));
  std::ranges::fill(leases[0].Bytes(), 'a');
  std::ranges::fill(leases[1].Bytes(), 'b');
  std::ranges::fill(leases[2].Bytes(), '\0');

  const auto writes = std::array<tinydb::cache::DirectIoCheckpointPage, 2>{
      tinydb::cache::DirectIoCheckpointPage{.page_id = 2, .data = leases[0].Bytes().data()},
      tinydb::cache::DirectIoCheckpointPage{.page_id = 3, .data = leases[1].Bytes().data()},
  };
  const auto wrote = engine.WriteCheckpointPages(writes, logical_page_count);
  ASSERT_TRUE(wrote.has_value()) << wrote.error().ToString();
  ASSERT_TRUE(*wrote);

  auto pages = std::vector<tinydb::cache::PageArena::Lease>{};
  pages.push_back(std::move(leases[2]));
  auto read = engine.ScheduleExact({3}, std::move(pages));
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->Wait(), tinydb::cache::DirectReadRunState::Ready);
  auto loaded = read->TakePage(0);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(std::ranges::all_of(loaded.Bytes(), [](char byte) { return byte == 'b'; }));
  engine.DrainForTesting();
}

TEST(DirectIoEngine, ExactReadCompletionFailureIsReportedAndEngineDrains) {
  const auto database_file = ScopedDatabase("direct_engine_completion_failure");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  constexpr auto logical_page_count = tinydb::page_id_t{3};
  ASSERT_TRUE(disk->EnsurePageCount(logical_page_count).Ok());
  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, logical_page_count, 0).Ok());

  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  if (!engine.Available()) {
    GTEST_SKIP() << "native io_uring is unavailable; synchronous direct fallback remains active";
  }
  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(1);
  ASSERT_TRUE(arena->AcquireBatch(pages));
  const auto fail = [](void *, bool, tinydb::page_id_t, std::size_t, int *result) noexcept { *result = -EIO; };
  engine.SetCompletionHookForTest(fail, nullptr);

  auto read = engine.ScheduleExact({2}, std::move(pages));
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->Wait(), tinydb::cache::DirectReadRunState::Failed);
  EXPECT_FALSE(read->TakePage(0));
  engine.DrainForTesting();
}

TEST(DirectIoEngine, ExactRunOwnsAndTransfersReadyArenaPages) {
  const auto database_file = ScopedDatabase("direct_engine_exact_run");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  constexpr auto logical_page_count = tinydb::page_id_t{4};
  ASSERT_TRUE(disk->EnsurePageCount(logical_page_count).Ok());
  auto first = std::array<char, tinydb::PAGE_SIZE>{};
  auto second = std::array<char, tinydb::PAGE_SIZE>{};
  std::ranges::fill(first, 'x');
  std::ranges::fill(second, 'y');
  ASSERT_TRUE(WriteCheckpointPage(*disk, 2, first.data(), logical_page_count).Ok());
  ASSERT_TRUE(WriteCheckpointPage(*disk, 3, second.data(), logical_page_count).Ok());
  ASSERT_TRUE(disk->Sync().Ok());
  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, logical_page_count, 0).Ok());

  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  if (!engine.Available()) {
    GTEST_SKIP() << "native io_uring is unavailable; synchronous direct fallback remains active";
  }
  auto arena = tinydb::cache::PageArena::CreateDirect(2);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(2);
  ASSERT_TRUE(arena->AcquireBatch(pages));
  auto run = engine.ScheduleExact({2, 3}, std::move(pages));
  ASSERT_NE(run, nullptr);

  EXPECT_EQ(run->Wait(), tinydb::cache::DirectReadRunState::Ready);
  EXPECT_EQ(run->Wait(), tinydb::cache::DirectReadRunState::Ready);
  auto first_transferred = run->TakePage(0);
  ASSERT_TRUE(first_transferred);
  EXPECT_TRUE(std::ranges::all_of(first_transferred.Bytes(), [](char value) { return value == 'x'; }));
  auto transferred = run->TakePage(1);
  ASSERT_TRUE(transferred);
  EXPECT_TRUE(std::ranges::all_of(transferred.Bytes(), [](char value) { return value == 'y'; }));
  EXPECT_FALSE(run->TakePage(1));

  run->Cancel();
  EXPECT_EQ(run->Wait(), tinydb::cache::DirectReadRunState::Cancelled);
  EXPECT_FALSE(run->TakePage(0));
  engine.DrainForTesting();
}

TEST(DirectIoEngine, ExactRunTransfersScatteredPagesOnlyAfterReady) {
  constexpr auto page_count = std::size_t{16};
  constexpr auto logical_page_count = tinydb::page_id_t{33};
  const auto database_file = ScopedDatabase("direct_engine_exact_scattered");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  ASSERT_TRUE(disk->EnsurePageCount(logical_page_count).Ok());

  auto page_ids = std::vector<tinydb::page_id_t>{};
  page_ids.reserve(page_count);
  for (auto index = std::size_t{0}; index < page_count; ++index) {
    const auto page_id = tinydb::FIRST_DATA_PAGE_ID + 2U * index;
    page_ids.push_back(page_id);
    auto stored = std::array<char, tinydb::PAGE_SIZE>{};
    std::ranges::fill(stored, static_cast<char>('a' + index));
    ASSERT_TRUE(WriteCheckpointPage(*disk, page_id, stored.data(), logical_page_count).Ok());
  }
  ASSERT_TRUE(disk->Sync().Ok());
  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, logical_page_count, 0).Ok());

  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  if (!engine.Available()) {
    GTEST_SKIP() << "native io_uring is unavailable; synchronous direct fallback remains active";
  }
  auto completion = BlockingCompletion{};
  engine.SetCompletionHookForTest(BlockCompletion, &completion);
  auto arena = tinydb::cache::PageArena::CreateDirect(page_count);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(page_count);
  ASSERT_TRUE(arena->AcquireBatch(pages));
  auto run = engine.ScheduleExact(page_ids, std::move(pages));
  ASSERT_NE(run, nullptr);

  auto completion_entered = false;
  {
    auto lock = std::unique_lock(completion.mutex);
    completion_entered =
        completion.changed.wait_for(lock, std::chrono::seconds(5), [&completion] { return completion.entered; });
  }
  if (!completion_entered) {
    ReleaseCompletion(&completion);
    FAIL() << "scattered exact read did not reach completion";
  }
  EXPECT_EQ(run->State(), tinydb::cache::DirectReadRunState::Loading);
  EXPECT_FALSE(run->TakePage(0));

  ReleaseCompletion(&completion);
  ASSERT_EQ(run->Wait(), tinydb::cache::DirectReadRunState::Ready);
  engine.SetCompletionHookForTest(nullptr, nullptr);
  for (auto index = std::size_t{0}; index < page_count; ++index) {
    auto transferred = run->TakePage(index);
    ASSERT_TRUE(transferred);
    const auto expected = static_cast<char>('a' + index);
    EXPECT_TRUE(std::ranges::all_of(transferred.Bytes(), [expected](char value) { return value == expected; }));
  }
  engine.DrainForTesting();
}

TEST(DirectIoEngine, ExactRunCancellationSkipsQueuedIoAndDrainsActiveIo) {
  const auto database_file = ScopedDatabase("direct_engine_exact_cancel");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  constexpr auto logical_page_count = tinydb::page_id_t{4};
  ASSERT_TRUE(disk->EnsurePageCount(logical_page_count).Ok());
  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, logical_page_count, 0).Ok());
  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  if (!engine.Available()) {
    GTEST_SKIP() << "native io_uring is unavailable; synchronous direct fallback remains active";
  }

  auto completion = BlockingCompletion{};
  engine.SetCompletionHookForTest(BlockCompletion, &completion);
  auto arena = tinydb::cache::PageArena::CreateDirect(2);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(2);
  ASSERT_TRUE(arena->AcquireBatch(pages));
  auto first_pages = std::vector<tinydb::cache::PageArena::Lease>{};
  auto second_pages = std::vector<tinydb::cache::PageArena::Lease>{};
  first_pages.push_back(std::move(pages[0]));
  second_pages.push_back(std::move(pages[1]));
  auto active = engine.ScheduleExact({2}, std::move(first_pages));
  ASSERT_NE(active, nullptr);
  {
    auto lock = std::unique_lock(completion.mutex);
    ASSERT_TRUE(
        completion.changed.wait_for(lock, std::chrono::seconds(5), [&completion] { return completion.entered; }));
  }
  EXPECT_EQ(active->State(), tinydb::cache::DirectReadRunState::Loading);
  active->Cancel();
  EXPECT_EQ(active->State(), tinydb::cache::DirectReadRunState::Loading);

  auto queued = engine.ScheduleExact({3}, std::move(second_pages));
  ASSERT_NE(queued, nullptr);
  queued->Cancel();
  EXPECT_EQ(queued->Wait(), tinydb::cache::DirectReadRunState::Cancelled);
  EXPECT_EQ(queued->Wait(), tinydb::cache::DirectReadRunState::Cancelled);

  ReleaseCompletion(&completion);
  EXPECT_EQ(active->Wait(), tinydb::cache::DirectReadRunState::Cancelled);
  engine.DrainForTesting();
  {
    auto lock = std::lock_guard(completion.mutex);
    EXPECT_EQ(completion.calls, 1U);
  }
}

TEST(DirectIoEngine, ExactRunCanOutliveEngineShutdown) {
  const auto database_file = ScopedDatabase("direct_engine_exact_shutdown");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  constexpr auto logical_page_count = tinydb::page_id_t{3};
  ASSERT_TRUE(disk->EnsurePageCount(logical_page_count).Ok());
  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, logical_page_count, 0).Ok());
  auto engine = std::make_unique<tinydb::cache::DirectIoEngine>(&*disk);
  if (!engine->Available()) {
    GTEST_SKIP() << "native io_uring is unavailable; synchronous direct fallback remains active";
  }
  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  auto pages = std::vector<tinydb::cache::PageArena::Lease>(1);
  ASSERT_TRUE(arena->AcquireBatch(pages));
  auto run = engine->ScheduleExact({2}, std::move(pages));
  ASSERT_NE(run, nullptr);

  engine.reset();
  const auto state = run->Wait();
  EXPECT_TRUE(state == tinydb::cache::DirectReadRunState::Ready ||
              state == tinydb::cache::DirectReadRunState::Cancelled);
  run->Cancel();
}

TEST(DirectIoEngine, RejectsInvalidCheckpointPlansBeforeBackendSelection) {
  const auto database_file = ScopedDatabase("direct_engine_invalid_plan");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Buffered);
  ASSERT_TRUE(disk.has_value()) << disk.error().ToString();
  auto engine = tinydb::cache::DirectIoEngine(&*disk);
  auto page = std::array<char, tinydb::PAGE_SIZE>{};

  const auto duplicate = std::array<tinydb::cache::DirectIoCheckpointPage, 2>{
      tinydb::cache::DirectIoCheckpointPage{.page_id = 2, .data = page.data()},
      tinydb::cache::DirectIoCheckpointPage{.page_id = 2, .data = page.data()},
  };
  const auto result = engine.WriteCheckpointPages(duplicate, 4);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::InvalidArgument);
}

TEST(DirectIoEngine, DiskManagerPreparesRequestsAgainstLogicalBoundaries) {
  const auto database_file = ScopedDatabase("direct_engine_logical_boundaries");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Direct);
  if (!disk) {
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << disk.error().ToString();
  }
  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  auto page = arena->Acquire();
  ASSERT_TRUE(page);
  auto bytes = std::as_writable_bytes(std::span{page.Bytes()});
  const auto destination = std::span<std::byte>{bytes.data(), bytes.size()};
  const auto page_ids = std::array<tinydb::page_id_t, 1>{tinydb::FIRST_DATA_PAGE_ID};

  ASSERT_TRUE(disk->EnsurePageCount(3).Ok());
  auto read = disk->BeginDirectReadPages(page_ids, destination);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().Code(), tinydb::StatusCode::InvalidArgument);

  ASSERT_TRUE(disk->CommitCheckpoint(0, 0, 3, 0).Ok());
  read = disk->BeginDirectReadPages(page_ids, destination);
  ASSERT_TRUE(read.has_value()) << read.error().ToString();
  const auto read_result = std::array<int, 1>{static_cast<int>(tinydb::PAGE_SIZE)};
  const auto expected = std::array<std::size_t, 1>{tinydb::PAGE_SIZE};
  EXPECT_TRUE(std::move(*read).Complete(read_result, expected).Ok());

  const auto vector = iovec{.iov_base = page.Bytes().data(), .iov_len = tinydb::PAGE_SIZE};
  auto write = disk->BeginDirectCheckpointWrite(2, std::span{&vector, 1U}, 2);
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error().Code(), tinydb::StatusCode::InvalidArgument);
  write = disk->BeginDirectCheckpointWrite(2, std::span{&vector, 1U}, 3);
  ASSERT_TRUE(write.has_value()) << write.error().ToString();
  EXPECT_TRUE(std::move(*write).Complete(static_cast<int>(tinydb::PAGE_SIZE), tinydb::PAGE_SIZE).Ok());
}

TEST(DirectIoEngine, DiskReadsRaceSafelyWithLogicalFrontierPublication) {
  const auto database_file = ScopedDatabase("disk_logical_frontier_concurrency");
  auto disk = tinydb::DiskManager::Open(database_file.Path(), tinydb::PageIoMode::Buffered);
  ASSERT_TRUE(disk.has_value()) << disk.error().ToString();
  ASSERT_TRUE(disk->EnsurePageCount(3).Ok());
  auto stored = std::array<char, tinydb::PAGE_SIZE>{};
  std::ranges::fill(stored, 'p');
  ASSERT_TRUE(WriteCheckpointPage(*disk, 2, stored.data(), 3).Ok());
  ASSERT_TRUE(disk->Sync().Ok());

  auto stop = std::atomic<bool>{false};
  auto invalid_status = std::atomic<bool>{false};
  auto reader = std::thread([&] {
    auto page = std::array<std::byte, tinydb::PAGE_SIZE>{};
    while (!stop.load(std::memory_order_acquire)) {
      const auto status = disk->ReadPage(2, page);
      if (!status.Ok() && status.Code() != tinydb::StatusCode::InvalidArgument) {
        invalid_status.store(true, std::memory_order_release);
        return;
      }
    }
  });

  const auto committed = disk->CommitCheckpoint(0, 0, 3, 0);
  stop.store(true, std::memory_order_release);
  reader.join();
  ASSERT_TRUE(committed.Ok()) << committed.ToString();
  EXPECT_FALSE(invalid_status.load(std::memory_order_acquire));
  auto loaded = std::array<std::byte, tinydb::PAGE_SIZE>{};
  ASSERT_TRUE(disk->ReadPage(2, loaded).Ok());
  EXPECT_TRUE(std::ranges::all_of(loaded, [](std::byte byte) { return byte == std::byte{'p'}; }));
}

}  // namespace
