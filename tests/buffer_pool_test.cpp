#include <gtest/gtest.h>
#include <tinydb/buffer_pool.h>
#include <tinydb/status.h>

#include "storage/page_codec.h"

#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_buffer_" + name + "_" + std::to_string(::getpid()) + ".db");
}

static void StoreMarker(char *data, tinydb::page_id_t page_id, char marker) {
  // Buffer-pool tests care about caching, not a particular tree layout. A tiny
  // overflow page provides codec-valid opaque content whose marker survives
  // flush/eviction and whose embedded page identity can be checked on read.
  const auto payload = std::array{static_cast<std::byte>(marker)};
  const auto encoded = tinydb::storage::EncodeOverflowPage(page_id, 0, payload.size(), tinydb::HEADER_PAGE_ID, payload);
  ASSERT_TRUE(encoded.has_value());
  std::memcpy(data, encoded->data(), encoded->size());
}

static auto ReadMarker(const char *data, tinydb::page_id_t page_id) -> char {
  // Decoding here also proves eviction wrote a complete checksummed page rather
  // than merely preserving the one byte the old tests used to inspect.
  const auto decoded = tinydb::storage::DecodeOverflowPage(std::as_bytes(std::span{data, tinydb::PAGE_SIZE}), page_id);
  EXPECT_TRUE(decoded.has_value());
  return static_cast<char>(std::to_integer<unsigned char>(decoded->payload.front()));
}

TEST(BufferPoolTest, FlushNewPage) {
  const auto path = TestPath("flush");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    const auto [page_id, data] = pool.NewPage().value();
    StoreMarker(data, page_id, 'b');

    pool.UnpinPage(page_id, true);
    ASSERT_TRUE(pool.FlushAllPages().Ok());

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    ASSERT_TRUE(disk.ReadPage(page_id, page.data()).Ok());

    EXPECT_EQ(ReadMarker(page.data(), page_id), 'b');
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, EvictDirtyPage) {
  const auto path = TestPath("evict");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    const auto [first_page_id, first_page] = pool.NewPage().value();
    StoreMarker(first_page, first_page_id, 'a');
    pool.UnpinPage(first_page_id, true);

    const auto [second_page_id, second_page] = pool.NewPage().value();
    StoreMarker(second_page, second_page_id, 'z');
    pool.UnpinPage(second_page_id, true);

    char *fetched_page = pool.FetchPage(first_page_id).value();
    EXPECT_EQ(ReadMarker(fetched_page, first_page_id), 'a');
    pool.UnpinPage(first_page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, FreedPageIsForgottenAndReused) {
  const auto path = TestPath("free");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 2);

    const auto [page_id, data] = pool.NewPage().value();
    data[0] = 'x';
    pool.UnpinPage(page_id, true);
    pool.FreePage(page_id);  // must discard the dirty bytes with the page

    const auto [reused_id, reused] = pool.NewPage().value();
    EXPECT_EQ(reused_id, page_id);  // the freed page comes back
    EXPECT_EQ(reused[0], '\0');     // zeroed, not the stale 'x'
    pool.UnpinPage(reused_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolDeathTest, FreeOfPinnedPageDies) {
  const auto path = TestPath("free_pinned");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    const auto page_id = pool.NewPage().value().page_id;  // stays pinned

    EXPECT_DEATH(pool.FreePage(page_id), "freeing a pinned page");

    pool.UnpinPage(page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolDeathTest, UnpinOfNonResidentPageDies) {
  const auto path = TestPath("unpin_missing");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    EXPECT_DEATH(pool.UnpinPage(tinydb::FIRST_DATA_PAGE_ID, false), "not in the pool");
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolDeathTest, DoubleUnpinDies) {
  const auto path = TestPath("double_unpin");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    const auto page_id = pool.NewPage().value().page_id;
    pool.UnpinPage(page_id, true);

    EXPECT_DEATH(pool.UnpinPage(page_id, false), "unpinning an unpinned page");
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, KeepPinnedPage) {
  const auto path = TestPath("pinned");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    const auto page_id = pool.NewPage().value().page_id;

    // Every frame is pinned, so there is nothing to evict.
    const auto blocked = pool.NewPage();
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error().Code(), tinydb::StatusCode::ResourceExhausted);

    pool.UnpinPage(page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, FailedFetchDoesNotPoisonThePageTable) {
  const auto path = TestPath("failed_fetch");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 2);

    // Two pages on disk, both cached, both frames occupied and clean.
    const auto page_a = pool.NewPage().value().page_id;
    StoreMarker(pool.FetchPage(page_a).value(), page_a, 'a');
    pool.UnpinPage(page_a, true);
    pool.UnpinPage(page_a, true);
    const auto page_b = pool.NewPage().value().page_id;
    StoreMarker(pool.FetchPage(page_b).value(), page_b, 'b');
    pool.UnpinPage(page_b, true);
    pool.UnpinPage(page_b, true);
    ASSERT_TRUE(pool.FlushAllPages().Ok());

    // A fetch of an unallocated page evicts one occupant and then fails,
    // leaving the picked frame with no page. The frame must go back to
    // the free list — not sit abandoned still recorded under the evicted
    // page's id.
    const auto failed = pool.FetchPage(page_b + 100);
    ASSERT_FALSE(failed.has_value());

    // Re-cache the evicted page and hold it pinned with fresh bytes.
    auto *const b_data = pool.FetchPage(page_b).value();
    StoreMarker(b_data, page_b, 'c');

    // Fetching the other page needs a frame. Before the fix, the sweep
    // reused the abandoned frame and erased its stale id — the *live*
    // mapping of the pinned page above — from the page table, leaving the
    // pool caching that page in two frames; this unpin then died with
    // "unpinning a page that is not in the pool".
    ASSERT_TRUE(pool.FetchPage(page_a).has_value());
    pool.UnpinPage(page_a, false);
    pool.UnpinPage(page_b, true);

    // The pinned page's bytes survived the shuffle.
    ASSERT_TRUE(pool.FlushAllPages().Ok());
    auto on_disk = std::array<char, tinydb::PAGE_SIZE>{};
    ASSERT_TRUE(disk.ReadPage(page_b, on_disk.data()).Ok());
    EXPECT_EQ(ReadMarker(on_disk.data(), page_b), 'c');
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, OpDirtyFramesAreNeitherEvictedNorFlushed) {
  const auto path = TestPath("no_steal");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    // Two pages on disk, pool of one frame.
    const auto first = pool.NewPage().value().page_id;
    StoreMarker(pool.FetchPage(first).value(), first, 'a');
    pool.UnpinPage(first, true);
    pool.UnpinPage(first, true);
    const auto second = pool.NewPage().value().page_id;  // evicts (writes) first
    StoreMarker(pool.FetchPage(second).value(), second, 'b');
    pool.UnpinPage(second, true);
    pool.UnpinPage(second, true);
    ASSERT_TRUE(pool.FlushAllPages().Ok());

    pool.BeginOp();
    auto *const data = pool.FetchPage(first).value();  // evicts second, clean
    StoreMarker(data, first, 'u');                     // uncommitted from here on
    pool.UnpinPage(first, true);

    // No-steal: the only frame holds uncommitted bytes, so nothing is
    // evictable even though nothing is pinned.
    const auto fetch = pool.FetchPage(second);
    ASSERT_FALSE(fetch.has_value());
    EXPECT_EQ(fetch.error().Code(), tinydb::StatusCode::ResourceExhausted);

    // And flushing skips it: the on-disk page keeps its committed bytes.
    ASSERT_TRUE(pool.FlushAllPages().Ok());
    auto on_disk = std::array<char, tinydb::PAGE_SIZE>{};
    ASSERT_TRUE(disk.ReadPage(first, on_disk.data()).Ok());
    EXPECT_EQ(ReadMarker(on_disk.data(), first), 'a');

    // The frame is exactly what the operation must log.
    const auto images = pool.OpDirtyFrames();
    ASSERT_EQ(images.size(), 1U);
    EXPECT_EQ(images[0].first, first);
    EXPECT_EQ(ReadMarker(images[0].second, first), 'u');

    // After EndOp the frame is ordinary dirty again: fetching the other
    // page evicts it, writing the bytes out on the way.
    pool.EndOp();
    ASSERT_TRUE(pool.FetchPage(second).has_value());
    pool.UnpinPage(second, false);
    ASSERT_TRUE(disk.ReadPage(first, on_disk.data()).Ok());
    EXPECT_EQ(ReadMarker(on_disk.data(), first), 'u');
  }

  std::filesystem::remove(path);
}
