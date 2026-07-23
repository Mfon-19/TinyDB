#include <gtest/gtest.h>

#include "cache/committed_page_cache.h"
#include "storage/disk_manager.h"
#include "storage/page_codec.h"
#include "support/test_files.h"

#include <cstddef>
#include <cstdint>
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
  const auto high_water = tinydb::FIRST_DATA_PAGE_ID + count;
  ASSERT_TRUE(disk.EnsurePageCount(high_water).Ok());
  for (auto page_id = tinydb::FIRST_DATA_PAGE_ID; page_id < high_water; ++page_id) {
    const auto encoded = tinydb::storage::EncodeFreeExtentPage(page_id, 1, tinydb::HEADER_PAGE_ID, {});
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(disk.WriteCheckpointPage(page_id, encoded->data(), high_water).Ok());
  }
  ASSERT_TRUE(disk.Sync().Ok());
  ASSERT_TRUE(disk.CommitCheckpoint(tinydb::FIRST_DATA_PAGE_ID, tinydb::HEADER_PAGE_ID, high_water, 1, 1).Ok());
}

auto Image(tinydb::page_id_t page_id, std::uint64_t page_lsn) -> tinydb::cache::CommittedPageImage {
  const auto encoded = tinydb::storage::EncodeFreeExtentPage(page_id, page_lsn, tinydb::HEADER_PAGE_ID, {});
  if (!encoded) {
    throw std::runtime_error(encoded.error().ToString());
  }
  return tinydb::cache::CommittedPageImage{
      .page_id = page_id,
      .page_lsn = page_lsn,
      .bytes = std::make_unique<tinydb::cache::PageBytes>(*encoded),
  };
}

}  // namespace

TEST(Cache, Lru) {
  const auto path = Path("cache_lru");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  WritePages(disk, 3);
  auto cache = tinydb::cache::CommittedPageCache(&disk, 2U * tinydb::PAGE_SIZE, 1);

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
  auto cache = tinydb::cache::CommittedPageCache(&disk, tinydb::PAGE_SIZE, 1);

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
  auto cache = tinydb::cache::CommittedPageCache(&disk, 2U * tinydb::PAGE_SIZE, 1);

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

TEST(Cache, Checkpoint) {
  const auto path = Path("cache_checkpoint");
  tinydb::test::Remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  auto cache = tinydb::cache::CommittedPageCache(&disk, tinydb::PAGE_SIZE, 0);

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
