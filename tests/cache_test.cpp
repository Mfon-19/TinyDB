#include <gtest/gtest.h>

#include "btree/leaf_page_builder.h"
#include "cache/committed_page_cache.h"
#include "storage/disk_manager.h"
#include "storage/page_codec.h"
#include "support/test_files.h"
#include "txn/transaction_pages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/*
** CACHE REPLACEMENT TESTS
**
** These cases use valid checkpoint pages but no B+ tree. That keeps page
** selection deterministic: each Read names the exact frame whose queue
** position is under test. Publication and checkpoint tests use the same page
** images as the production commit path.
*/
namespace {

auto Path(std::string_view name) { return tinydb::test::Path(name); }

void WritePages(tinydb::DiskManager &disk, std::size_t count) {
  const auto logical_page_count = tinydb::FIRST_DATA_PAGE_ID + count;
  ASSERT_TRUE(disk.EnsurePageCount(logical_page_count).Ok());
  for (auto page_id = tinydb::FIRST_DATA_PAGE_ID; page_id < logical_page_count; ++page_id) {
    const auto encoded = tinydb::storage::EncodeFreeExtentPage(page_id, 1, tinydb::HEADER_PAGE_ID, {});
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(disk.WriteCheckpointPage(page_id, encoded->data(), logical_page_count).Ok());
  }
  ASSERT_TRUE(disk.Sync().Ok());
  ASSERT_TRUE(disk.CommitCheckpoint(tinydb::FIRST_DATA_PAGE_ID, tinydb::HEADER_PAGE_ID, logical_page_count, 1).Ok());
}

auto EncodedLeaf(tinydb::page_id_t page_id) -> std::array<char, tinydb::PAGE_SIZE> {
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::LeafPageBuilder{};
  static_cast<void>(builder.Upsert("key", tinydb::LeafValueView::Inline("value")));
  builder.Store(page.data(), page_id);
  const auto sealed = tinydb::storage::RewriteDataPageLsn(std::as_writable_bytes(std::span{page}), page_id, 1);
  if (!sealed) {
    throw std::runtime_error(sealed.error().ToString());
  }
  return page;
}

void WriteLeafPages(tinydb::DiskManager &disk, std::size_t count) {
  const auto logical_page_count = tinydb::FIRST_DATA_PAGE_ID + count;
  ASSERT_TRUE(disk.EnsurePageCount(logical_page_count).Ok());
  for (auto page_id = tinydb::FIRST_DATA_PAGE_ID; page_id < logical_page_count; ++page_id) {
    const auto encoded = EncodedLeaf(page_id);
    ASSERT_TRUE(disk.WriteCheckpointPage(page_id, encoded.data(), logical_page_count).Ok());
  }
  ASSERT_TRUE(disk.Sync().Ok());
  ASSERT_TRUE(disk.CommitCheckpoint(tinydb::FIRST_DATA_PAGE_ID, tinydb::HEADER_PAGE_ID, logical_page_count, 1).Ok());
}

auto Image(tinydb::page_id_t page_id, std::uint64_t page_lsn,
           const std::shared_ptr<tinydb::cache::PageArena> &arena) -> tinydb::cache::CommittedPageImage {
  const auto encoded = tinydb::storage::EncodeFreeExtentPage(page_id, page_lsn, tinydb::HEADER_PAGE_ID, {});
  if (!encoded) {
    throw std::runtime_error(encoded.error().ToString());
  }
  const auto header = tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{*encoded}), page_id);
  if (!header) {
    throw std::runtime_error(header.error().ToString());
  }
  auto bytes = arena->Acquire();
  if (!bytes) {
    throw std::runtime_error("could not allocate cache-test page image");
  }
  std::ranges::copy(*encoded, bytes->begin());
  return tinydb::cache::CommittedPageImage{
      .header = *header,
      .bytes = std::move(bytes),
  };
}

}  // namespace

TEST(Cache, PublicationKeepsArenaPageAddressAndPayloadProofs) {
  const auto path = Path("cache_arena_publication");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  auto cache = tinydb::cache::CommittedPageCache(disk, 4U * tinydb::PAGE_SIZE, 0);
  auto arena = cache.SharedPageArena();
  auto transaction = tinydb::txn::TransactionPages::Begin(&cache,
                                                          tinydb::txn::DatabaseState{
                                                              .logical_page_count = tinydb::FIRST_DATA_PAGE_ID,
                                                          },
                                                          4U * tinydb::PAGE_SIZE, cache.SharedPageArena())
                         .value();

  const char *tree_address = nullptr;
  {
    auto page = transaction.Allocate().value();
    ASSERT_EQ(page.Id(), 2U);
    auto builder = tinydb::LeafPageBuilder{};
    builder.Store(page.MutableData(), page.Id());
    page.MarkTreePayloadValidated();
    page.MarkDirty();
    tree_address = page.Data();
  }

  const char *overflow_address = nullptr;
  {
    auto page = transaction.Allocate().value();
    ASSERT_EQ(page.Id(), 3U);
    const auto payload = std::array{std::byte{0x41}, std::byte{0x42}};
    ASSERT_TRUE(tinydb::storage::InitializeOverflowPage(
                    std::as_writable_bytes(std::span<char, tinydb::PAGE_SIZE>{page.MutableData(), tinydb::PAGE_SIZE}),
                    page.Id(), 0, page.Id(), 0, tinydb::HEADER_PAGE_ID, payload)
                    .Ok());
    page.MarkOverflowPayloadValidated();
    page.MarkDirty();
    overflow_address = page.Data();
  }

  {
    auto tree = transaction.Read(2).value();
    auto overflow = transaction.Read(3).value();
    EXPECT_EQ(tree.Data(), tree_address);
    EXPECT_TRUE(tree.TreePayloadValidated());
    EXPECT_EQ(overflow.Data(), overflow_address);
    EXPECT_TRUE(overflow.OverflowPayloadValidated());
  }

  ASSERT_TRUE(transaction.PrepareCommit(1).Ok());
  const auto state = transaction.ResultingState();
  auto images = transaction.TakePages();
  ASSERT_EQ(images.size(), 2U);
  EXPECT_EQ(images[0].bytes->data(), tree_address);
  EXPECT_TRUE(images[0].tree_payload_validated);
  EXPECT_EQ(images[1].bytes->data(), overflow_address);
  EXPECT_FALSE(images[1].tree_payload_validated);

  auto plan = cache.PreparePublication(std::move(images), {}, state.logical_page_count);
  ASSERT_TRUE(plan.has_value());
  cache.Publish(std::move(*plan));

  auto tree = cache.Read(2).value();
  auto overflow = cache.Read(3).value();
  EXPECT_EQ(tree.Data(), tree_address);
  EXPECT_TRUE(tree.TreePayloadValidated());
  EXPECT_EQ(overflow.Data(), overflow_address);
  ASSERT_NE(overflow.ValidatedHeader(), nullptr);
  EXPECT_EQ(overflow.ValidatedHeader()->type, tinydb::storage::DataPageType::Overflow);
  tinydb::test::Remove(path);
}

TEST(Cache, Lru) {
  const auto path = Path("cache_lru");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 3);
  auto cache = tinydb::cache::CommittedPageCache(disk, 2U * tinydb::PAGE_SIZE, 1);

  {
    auto first = cache.Read(2);  // [2]
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first->ValidatedHeader(), nullptr);
    EXPECT_EQ(first->ValidatedHeader()->page_id, 2U);
    EXPECT_EQ(first->ValidatedHeader()->page_lsn, 1U);
  }
  ASSERT_TRUE(cache.Read(3).has_value());  // [3, 2]
  ASSERT_TRUE(cache.Read(2).has_value());  // [2, 3]
  ASSERT_TRUE(cache.Read(4).has_value());  // evicts 3: [4, 2]
  ASSERT_TRUE(cache.Read(2).has_value());  // [2, 4]
  ASSERT_TRUE(cache.Read(3).has_value());  // evicts 4: [3, 2]

  const auto stats = cache.Stats();
  EXPECT_EQ(stats.hits, 2U);
  EXPECT_EQ(stats.misses, 4U);
  EXPECT_EQ(stats.evictions, 2U);
  EXPECT_EQ(stats.resident_pages, 2U);
  tinydb::test::Remove(path);
}

TEST(Cache, Pins) {
  const auto path = Path("cache_pins");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, tinydb::PAGE_SIZE, 1);

  {
    auto pinned = cache.Read(2);
    ASSERT_TRUE(pinned.has_value());
    const auto blocked = cache.Read(3);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error().Code(), tinydb::StatusCode::ResourceExhausted);
  }

  ASSERT_TRUE(cache.Read(3).has_value());
  const auto stats = cache.Stats();
  EXPECT_EQ(stats.evictions, 1U);
  EXPECT_EQ(stats.resident_pages, 1U);
  tinydb::test::Remove(path);
}

TEST(Cache, PinnedLruTailDoesNotBlockAnotherVictim) {
  const auto path = Path("cache_pinned_lru_tail");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 3);
  auto cache = tinydb::cache::CommittedPageCache(disk, 2U * tinydb::PAGE_SIZE, 1);

  auto pinned = cache.Read(2);
  ASSERT_TRUE(pinned.has_value());
  ASSERT_TRUE(cache.Read(3).has_value());
  ASSERT_TRUE(cache.Read(4).has_value());

  const auto stats = cache.Stats();
  EXPECT_EQ(stats.evictions, 1U);
  EXPECT_EQ(stats.resident_pages, 2U);
  EXPECT_EQ(stats.pinned_pages, 1U);
  EXPECT_TRUE(cache.Read(2).has_value());
  tinydb::test::Remove(path);
}

TEST(Cache, DifferentMissesLoadConcurrently) {
  using namespace std::chrono_literals;

  const auto path = Path("cache_parallel_misses");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, 2U * tinydb::PAGE_SIZE, 1);

  auto page2_entered = std::promise<void>{};
  auto page3_entered = std::promise<void>{};
  auto release = std::promise<void>{};
  auto page2_ready = page2_entered.get_future();
  auto page3_ready = page3_entered.get_future();
  const auto released = release.get_future().share();
  auto saw_page2 = std::atomic{false};
  auto saw_page3 = std::atomic{false};

  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall != tinydb::io::Syscall::Pread || call.path != path) {
          return std::nullopt;
        }
        if (call.offset == 2U * tinydb::PAGE_SIZE && !saw_page2.exchange(true)) {
          page2_entered.set_value();
          released.wait();
        } else if (call.offset == 3U * tinydb::PAGE_SIZE && !saw_page3.exchange(true)) {
          page3_entered.set_value();
          released.wait();
        }
        return std::nullopt;
      },
      [&] {
        auto first = std::async(std::launch::async, [&] { return cache.Read(2); });
        EXPECT_EQ(page2_ready.wait_for(2s), std::future_status::ready);
        auto second = std::async(std::launch::async, [&] { return cache.Read(3); });

        // Page 2 remains blocked inside pread. Reaching page 3's hook proves
        // its miss did not wait for the first miss to release the cache mutex.
        const auto second_entered_while_first_blocked = page3_ready.wait_for(2s) == std::future_status::ready;
        release.set_value();

        EXPECT_TRUE(second_entered_while_first_blocked);
        EXPECT_TRUE(first.get().has_value());
        EXPECT_TRUE(second.get().has_value());
      });

  const auto stats = cache.Stats();
  EXPECT_EQ(stats.misses, 2U);
  EXPECT_EQ(stats.resident_pages, 2U);
  tinydb::test::Remove(path);
}

TEST(Cache, SamePageMissesShareOnePhysicalRead) {
  using namespace std::chrono_literals;

  const auto path = Path("cache_coalesced_miss");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 1);
  auto cache = tinydb::cache::CommittedPageCache(disk, tinydb::PAGE_SIZE, 1);

  auto first_read_entered = std::promise<void>{};
  auto release = std::promise<void>{};
  auto entered = first_read_entered.get_future();
  const auto released = release.get_future().share();
  auto physical_reads = std::atomic<std::size_t>{0};

  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pread && call.path == path && call.offset == 2U * tinydb::PAGE_SIZE) {
          if (physical_reads.fetch_add(1) == 0) {
            first_read_entered.set_value();
          }
          released.wait();
        }
        return std::nullopt;
      },
      [&] {
        auto first = std::async(std::launch::async, [&] { return cache.Read(2); });
        const auto first_started = entered.wait_for(2s) == std::future_status::ready;
        auto second = std::async(std::launch::async, [&] { return cache.Read(2); });

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (cache.Stats().misses != 2U && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::yield();
        }
        const auto second_joined = cache.Stats().misses == 2U;
        release.set_value();

        EXPECT_TRUE(first_started);
        EXPECT_TRUE(second_joined);
        EXPECT_TRUE(first.get().has_value());
        EXPECT_TRUE(second.get().has_value());
      });

  EXPECT_EQ(physical_reads.load(), 1U);
  EXPECT_EQ(cache.Stats().resident_pages, 1U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectDemandReadUsesSynchronousPageIo) {
  const auto path = Path("cache_direct_demand");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, 2U * tinydb::PAGE_SIZE, 1);

  auto synchronous_reads = std::size_t{0};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pread && call.path == path && call.offset == 2U * tinydb::PAGE_SIZE) {
          ++synchronous_reads;
        }
        return std::nullopt;
      },
      [&] {
        const auto page = cache.Read(2);
        ASSERT_TRUE(page.has_value()) << page.error().ToString();
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(page->Data()) % tinydb::PAGE_SIZE, 0U);
      });

  EXPECT_EQ(synchronous_reads, 1U);
  EXPECT_TRUE(cache.Read(3).has_value());
  tinydb::test::Remove(path);
}

TEST(Cache, BufferedReadStreamMovesAndFallsBackAfterRepeatedCancellation) {
  const auto path = Path("cache_buffered_read_stream");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 1);
  auto cache = tinydb::cache::CommittedPageCache(disk, tinydb::PAGE_SIZE, 1);

  auto original = cache.BeginReadStream();
  EXPECT_FALSE(original.AcceptsExactPagePlan());
  auto stream = std::move(original);

  stream.PrimeExactPages(std::array<tinydb::page_id_t, 1>{2});
  stream.CancelAdvice();
  stream.CancelAdvice();
  ASSERT_TRUE(stream.Read(2).has_value());

  auto replacement = cache.BeginReadStream();
  replacement = std::move(stream);
  EXPECT_TRUE(replacement.Read(2).has_value());
  const auto stats = cache.Stats();
  EXPECT_EQ(stats.misses, 1U);
  EXPECT_EQ(stats.hits, 1U);
  EXPECT_EQ(stats.read_ahead_plans, 0U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectReadStreamFiltersFrontierAndResidentHintsAndAuthenticatesTreePages) {
  const auto path = Path("cache_direct_read_stream_filter");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  WriteLeafPages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, 16U * tinydb::PAGE_SIZE, 1);
  ASSERT_TRUE(cache.Read(2).has_value());

  auto stream = cache.BeginReadStream();
  if (!stream.AcceptsExactPagePlan()) {
    tinydb::test::Remove(path);
    GTEST_SKIP() << "native io_uring is unavailable; direct read advice remains disabled";
  }
  const auto hints = std::array<tinydb::page_id_t, 5>{
      tinydb::HEADER_PAGE_ID, 2, 3, disk.LogicalPageCount(), std::numeric_limits<tinydb::page_id_t>::max(),
  };
  stream.PrimeExactPages(hints);

  const auto staged = stream.Read(3);
  ASSERT_TRUE(staged.has_value()) << staged.error().ToString();
  ASSERT_NE(staged->ValidatedHeader(), nullptr);
  EXPECT_EQ(staged->ValidatedHeader()->page_id, 3U);
  EXPECT_TRUE(staged->TreePayloadValidated());
  EXPECT_TRUE(stream.Read(2).has_value());

  const auto stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 1U);
  EXPECT_EQ(stats.misses, 2U);
  EXPECT_EQ(stats.hits, 1U);
  EXPECT_EQ(stats.read_ahead_plans, 1U);
  EXPECT_EQ(stats.read_ahead_pages_scheduled, 1U);
  EXPECT_EQ(stats.read_ahead_pages_consumed, 1U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectReadStreamCancellationIsIdempotentAndSemanticReadsContinue) {
  const auto path = Path("cache_direct_read_stream_cancel");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  WritePages(disk, 1);
  auto cache = tinydb::cache::CommittedPageCache(disk, 16U * tinydb::PAGE_SIZE, 1);

  auto stream = cache.BeginReadStream();
  if (!stream.AcceptsExactPagePlan()) {
    tinydb::test::Remove(path);
    GTEST_SKIP() << "native io_uring is unavailable; direct read advice remains disabled";
  }
  stream.PrimeExactPages(std::array<tinydb::page_id_t, 1>{2});
  stream.CancelAdvice();
  stream.CancelAdvice();
  EXPECT_FALSE(stream.AcceptsExactPagePlan());
  ASSERT_TRUE(stream.Read(2).has_value());
  const auto stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 1U);
  EXPECT_EQ(stats.misses, 1U);
  EXPECT_EQ(stats.read_ahead_pages_consumed, 0U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectReadStreamSchedulingFailureUsesOrdinaryDemandRead) {
  const auto path = Path("cache_direct_read_stream_schedule_failure");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  WritePages(disk, 1);
  auto cache = tinydb::cache::CommittedPageCache(disk, 16U * tinydb::PAGE_SIZE, 1);
  auto stream = cache.BeginReadStream();
  if (!stream.AcceptsExactPagePlan()) {
    tinydb::test::Remove(path);
    GTEST_SKIP() << "native io_uring is unavailable; direct read advice remains disabled";
  }

  auto synchronous_reads = std::size_t{0};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pread && call.path == path && call.offset == 2U * tinydb::PAGE_SIZE) {
          ++synchronous_reads;
        }
        return std::nullopt;
      },
      [&] {
        stream.PrimeExactPages(std::array<tinydb::page_id_t, 1>{2});
        ASSERT_TRUE(stream.Read(2).has_value());
      });

  const auto stats = cache.Stats();
  EXPECT_EQ(synchronous_reads, 1U);
  EXPECT_EQ(stats.resident_pages, 1U);
  EXPECT_EQ(stats.misses, 1U);
  EXPECT_EQ(stats.read_ahead_plans, 0U);
  EXPECT_EQ(stats.read_ahead_pages_consumed, 0U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectReadStreamTreatsCorruptAdviceAsNonAuthoritative) {
  const auto path = Path("cache_direct_read_stream_corrupt");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  ASSERT_TRUE(disk.EnsurePageCount(3).Ok());
  auto corrupt = EncodedLeaf(2);
  corrupt[128] ^= 0x01;
  ASSERT_TRUE(disk.WriteCheckpointPage(2, corrupt.data(), 3).Ok());
  ASSERT_TRUE(disk.Sync().Ok());
  ASSERT_TRUE(disk.CommitCheckpoint(2, tinydb::HEADER_PAGE_ID, 3, 1).Ok());
  auto cache = tinydb::cache::CommittedPageCache(disk, 16U * tinydb::PAGE_SIZE, 1);

  auto stream = cache.BeginReadStream();
  if (!stream.AcceptsExactPagePlan()) {
    tinydb::test::Remove(path);
    GTEST_SKIP() << "native io_uring is unavailable; direct read advice remains disabled";
  }
  stream.PrimeExactPages(std::array<tinydb::page_id_t, 1>{2});
  const auto result = stream.Read(2);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::Corruption);
  const auto stats = cache.Stats();
  EXPECT_EQ(stats.misses, 1U);
  EXPECT_EQ(stats.read_ahead_pages_scheduled, 1U);
  EXPECT_EQ(stats.read_ahead_pages_consumed, 0U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectReadStreamRepeatedPlansAndDestructionReleaseStagingBudget) {
  const auto path = Path("cache_direct_read_stream_repeat");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, 4U * tinydb::PAGE_SIZE, 1);

  {
    auto stream = cache.BeginReadStream();
    if (!stream.AcceptsExactPagePlan()) {
      tinydb::test::Remove(path);
      GTEST_SKIP() << "native io_uring is unavailable; direct read advice remains disabled";
    }
    const auto plan = std::array<tinydb::page_id_t, 1>{2};
    stream.PrimeExactPages(plan);
    stream.PrimeExactPages(plan);
  }

  auto stream = cache.BeginReadStream();
  ASSERT_TRUE(stream.AcceptsExactPagePlan());
  stream.PrimeExactPages(std::array<tinydb::page_id_t, 1>{3});
  ASSERT_TRUE(stream.Read(3).has_value());
  const auto stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 0U);
  EXPECT_EQ(stats.read_ahead_plans, 2U);
  EXPECT_EQ(stats.read_ahead_pages_scheduled, 2U);
  EXPECT_EQ(stats.read_ahead_pages_consumed, 1U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectReadStreamUsesPassThroughWhenStagingBudgetCannotOwnOnePage) {
  const auto path = Path("cache_direct_read_stream_tiny_budget");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  WritePages(disk, 1);
  auto cache = tinydb::cache::CommittedPageCache(disk, 3U * tinydb::PAGE_SIZE, 1);
  auto stream = cache.BeginReadStream();
  EXPECT_FALSE(stream.AcceptsExactPagePlan());
  stream.PrimeExactPages(std::array<tinydb::page_id_t, 1>{2});
  ASSERT_TRUE(stream.Read(2).has_value());
  const auto stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 1U);
  EXPECT_EQ(stats.misses, 1U);
  EXPECT_EQ(stats.read_ahead_plans, 0U);
  tinydb::test::Remove(path);
}

TEST(Cache, ActiveLoadReservesCapacity) {
  using namespace std::chrono_literals;

  const auto path = Path("cache_load_capacity");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, tinydb::PAGE_SIZE, 1);

  auto load_entered = std::promise<void>{};
  auto release = std::promise<void>{};
  auto entered = load_entered.get_future();
  const auto released = release.get_future().share();
  auto saw_load = std::atomic{false};
  auto page3_reads = std::atomic<std::size_t>{0};

  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall != tinydb::io::Syscall::Pread || call.path != path) {
          return std::nullopt;
        }
        if (call.offset == 2U * tinydb::PAGE_SIZE && !saw_load.exchange(true)) {
          load_entered.set_value();
          released.wait();
        } else if (call.offset == 3U * tinydb::PAGE_SIZE) {
          ++page3_reads;
        }
        return std::nullopt;
      },
      [&] {
        auto first = std::async(std::launch::async, [&] { return cache.Read(2); });
        const auto first_entered = entered.wait_for(2s) == std::future_status::ready;
        EXPECT_TRUE(first_entered);

        const auto blocked = cache.Read(3);
        EXPECT_FALSE(blocked.has_value());
        if (!blocked) {
          EXPECT_EQ(blocked.error().Code(), tinydb::StatusCode::ResourceExhausted);
        }
        EXPECT_EQ(page3_reads.load(), 0U);

        release.set_value();
        EXPECT_TRUE(first.get().has_value());
      });

  const auto stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 1U);
  tinydb::test::Remove(path);
}

TEST(Cache, FailedLoadReleasesCapacity) {
  const auto path = Path("cache_failed_load_capacity");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, tinydb::PAGE_SIZE, 1);

  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pread && call.path == path && call.offset == 2U * tinydb::PAGE_SIZE) {
          return tinydb::io::Fault{.error = EIO};
        }
        return std::nullopt;
      },
      [&] {
        const auto failed = cache.Read(2);
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().Code(), tinydb::StatusCode::IoError);
      });

  // The failed read must return its in-flight capacity reservation.
  EXPECT_TRUE(cache.Read(3).has_value());
  EXPECT_EQ(cache.Stats().resident_pages, 1U);
  tinydb::test::Remove(path);
}

TEST(Cache, Checkpoint) {
  const auto path = Path("cache_checkpoint");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  auto cache = tinydb::cache::CommittedPageCache(disk, tinydb::PAGE_SIZE, 0);

  auto images = std::vector<tinydb::cache::CommittedPageImage>{};
  images.push_back(Image(2, 1, cache.SharedPageArena()));
  images.push_back(Image(3, 1, cache.SharedPageArena()));
  auto plan = cache.PreparePublication(std::move(images), {}, 4);
  ASSERT_TRUE(plan.has_value());
  cache.Publish(std::move(*plan));

  auto stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 2U);
  EXPECT_EQ(stats.dirty_pages, 2U);
  EXPECT_EQ(stats.evictions, 0U);

  cache.MarkCheckpointed(1);
  stats = cache.Stats();
  EXPECT_EQ(stats.resident_pages, 1U);
  EXPECT_EQ(stats.dirty_pages, 0U);
  EXPECT_EQ(stats.evictions, 1U);
  EXPECT_TRUE(cache.Read(3).has_value());
  tinydb::test::Remove(path);
}

TEST(Cache, BufferedCheckpointWritesUseSynchronousPageIo) {
  const auto path = Path("cache_buffered_checkpoint_write");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 2);
  auto cache = tinydb::cache::CommittedPageCache(disk, 2U * tinydb::PAGE_SIZE, 1);

  auto images = std::vector<tinydb::cache::CommittedPageImage>{};
  images.push_back(Image(2, 2, cache.SharedPageArena()));
  images.push_back(Image(3, 2, cache.SharedPageArena()));
  auto plan = cache.PreparePublication(std::move(images), {}, 4);
  ASSERT_TRUE(plan.has_value());
  cache.Publish(std::move(*plan));
  auto pages = cache.CaptureDirtyPages();

  auto writes = std::size_t{0};
  auto write_status = tinydb::Status{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == path && call.offset >= 2U * tinydb::PAGE_SIZE) {
          ++writes;
        }
        return std::nullopt;
      },
      [&] { write_status = cache.WriteCheckpointPages(pages, 4); });

  EXPECT_TRUE(write_status.Ok()) << write_status.ToString();
  EXPECT_EQ(writes, 2U);
  tinydb::test::Remove(path);
}

TEST(Cache, DirectCheckpointWritesUseAlignedFramesAndKeepSynchronousFallback) {
  constexpr auto checkpoint_pages = std::size_t{40};
  const auto path = Path("cache_direct_checkpoint_write");
  tinydb::test::Remove(path);
  auto opened = tinydb::DiskManager::Open(path, tinydb::PageIoMode::Direct);
  if (!opened) {
    const auto error = opened.error().ToString();
    tinydb::test::Remove(path);
    GTEST_SKIP() << "direct I/O is unavailable on the test filesystem: " << error;
  }
  auto disk = std::move(*opened);
  ASSERT_TRUE(disk.EnsurePageCount(tinydb::FIRST_DATA_PAGE_ID + checkpoint_pages).Ok());
  auto cache = tinydb::cache::CommittedPageCache(disk, checkpoint_pages * tinydb::PAGE_SIZE, 1);

  auto images = std::vector<tinydb::cache::CommittedPageImage>{};
  for (auto index = std::size_t{0}; index < checkpoint_pages; ++index) {
    images.push_back(Image(tinydb::FIRST_DATA_PAGE_ID + index, 2, cache.SharedPageArena()));
  }
  const auto logical_page_count = tinydb::FIRST_DATA_PAGE_ID + checkpoint_pages;
  auto plan = cache.PreparePublication(std::move(images), {}, logical_page_count);
  ASSERT_TRUE(plan.has_value());
  cache.Publish(std::move(*plan));
  auto pages = cache.CaptureDirtyPages();
  ASSERT_EQ(pages.size(), checkpoint_pages);
  for (const auto &page : pages) {
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(page.Data()) % tinydb::PAGE_SIZE, 0U);
  }

  const auto first_write = cache.WriteCheckpointPages(pages, logical_page_count);
  ASSERT_TRUE(first_write.Ok()) << first_write.ToString();
  cache.DrainIoForTesting();

  auto synchronous_batches = std::size_t{0};
  auto synchronous_pages = std::size_t{0};
  auto largest_batch = std::size_t{0};
  auto fallback_status = tinydb::Status{};
  tinydb::test::WithHook(
      [&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pwritev && call.path == path &&
            call.offset >= 2U * tinydb::PAGE_SIZE) {
          ++synchronous_batches;
          synchronous_pages += call.Vectors().size();
          largest_batch = std::max(largest_batch, call.Vectors().size());
          for (const auto &vector : call.Vectors()) {
            EXPECT_EQ(vector.iov_len, tinydb::PAGE_SIZE);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(vector.iov_base) % tinydb::PAGE_SIZE, 0U);
          }
        }
        return std::nullopt;
      },
      [&] { fallback_status = cache.WriteCheckpointPages(pages, logical_page_count); });

  EXPECT_TRUE(fallback_status.Ok()) << fallback_status.ToString();
  EXPECT_EQ(synchronous_batches, 2U);
  EXPECT_EQ(synchronous_pages, checkpoint_pages);
  EXPECT_EQ(largest_batch, tinydb::io::MAX_PAGE_WRITE_BATCH_PAGES);
  tinydb::test::Remove(path);
}
