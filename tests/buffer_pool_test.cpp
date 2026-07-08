#include <gtest/gtest.h>
#include <tinydb/buffer_pool.h>
#include <tinydb/status.h>

#include <unistd.h>

#include <array>
#include <filesystem>
#include <string>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_buffer_" + name + "_" + std::to_string(::getpid()) + ".db");
}

TEST(BufferPoolTest, FlushNewPage) {
  const auto path = TestPath("flush");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    const auto [page_id, data] = pool.NewPage().value();
    data[0] = 'b';
    data[tinydb::PAGE_SIZE - 1] = 'p';

    pool.UnpinPage(page_id, true);
    ASSERT_TRUE(pool.FlushAllPages().Ok());

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    ASSERT_TRUE(disk.ReadPage(page_id, page.data()).Ok());

    EXPECT_EQ(page[0], 'b');
    EXPECT_EQ(page[tinydb::PAGE_SIZE - 1], 'p');
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
    first_page[0] = 'a';
    pool.UnpinPage(first_page_id, true);

    const auto [second_page_id, second_page] = pool.NewPage().value();
    second_page[0] = 'z';
    pool.UnpinPage(second_page_id, true);

    char *fetched_page = pool.FetchPage(first_page_id).value();
    EXPECT_EQ(fetched_page[0], 'a');
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

TEST(BufferPoolTest, OpDirtyFramesAreNeitherEvictedNorFlushed) {
  const auto path = TestPath("no_steal");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    tinydb::BufferPool pool(&disk, 1);

    // Two pages on disk, pool of one frame.
    const auto first = pool.NewPage().value().page_id;
    pool.UnpinPage(first, true);
    const auto second = pool.NewPage().value().page_id;  // evicts (writes) first
    pool.UnpinPage(second, true);
    ASSERT_TRUE(pool.FlushAllPages().Ok());

    pool.BeginOp();
    auto *const data = pool.FetchPage(first).value();  // evicts second, clean
    data[0] = 'u';                                    // uncommitted from here on
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
    EXPECT_EQ(on_disk[0], '\0');

    // The frame is exactly what the operation must log.
    const auto images = pool.OpDirtyFrames();
    ASSERT_EQ(images.size(), 1U);
    EXPECT_EQ(images[0].first, first);
    EXPECT_EQ(images[0].second[0], 'u');

    // After EndOp the frame is ordinary dirty again: fetching the other
    // page evicts it, writing the bytes out on the way.
    pool.EndOp();
    ASSERT_TRUE(pool.FetchPage(second).has_value());
    pool.UnpinPage(second, false);
    ASSERT_TRUE(disk.ReadPage(first, on_disk.data()).Ok());
    EXPECT_EQ(on_disk[0], 'u');
  }

  std::filesystem::remove(path);
}
