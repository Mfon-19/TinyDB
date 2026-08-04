#include <gtest/gtest.h>

#include "cache/committed_page_cache.h"
#include "storage/disk_manager.h"
#include "storage/page_codec.h"
#include "support/test_files.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
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

auto Image(tinydb::page_id_t page_id, std::uint64_t page_lsn) -> tinydb::cache::CommittedPageImage {
  const auto encoded = tinydb::storage::EncodeFreeExtentPage(page_id, page_lsn, tinydb::HEADER_PAGE_ID, {});
  if (!encoded) {
    throw std::runtime_error(encoded.error().ToString());
  }
  const auto header = tinydb::storage::DecodeDataPageHeader(std::as_bytes(std::span{*encoded}), page_id);
  if (!header) {
    throw std::runtime_error(header.error().ToString());
  }
  return tinydb::cache::CommittedPageImage{
      .header = *header,
      .bytes = std::make_unique<tinydb::cache::PageBytes>(*encoded),
  };
}

}  // namespace

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
  images.push_back(Image(2, 1));
  images.push_back(Image(3, 1));
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
