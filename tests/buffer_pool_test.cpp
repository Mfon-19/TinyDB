#include <gtest/gtest.h>
#include <tinydb/buffer_pool.h>

#include <unistd.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_buffer_" + name + "_" + std::to_string(::getpid()) + ".db");
}

TEST(BufferPoolTest, FlushNewPage) {
  const auto path = TestPath("flush");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    const auto [page_id, data] = pool.NewPage();
    data[0] = 'b';
    data[tinydb::PAGE_SIZE - 1] = 'p';

    pool.UnpinPage(page_id, true);
    pool.FlushAllPages();

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    disk.ReadPage(page_id, page.data());

    EXPECT_EQ(page[0], 'b');
    EXPECT_EQ(page[tinydb::PAGE_SIZE - 1], 'p');
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, EvictDirtyPage) {
  const auto path = TestPath("evict");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    const auto [first_page_id, first_page] = pool.NewPage();
    first_page[0] = 'a';
    pool.UnpinPage(first_page_id, true);

    const auto [second_page_id, second_page] = pool.NewPage();
    second_page[0] = 'z';
    pool.UnpinPage(second_page_id, true);

    char *fetched_page = pool.FetchPage(first_page_id);
    EXPECT_EQ(fetched_page[0], 'a');
    pool.UnpinPage(first_page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, FreedPageIsForgottenAndReused) {
  const auto path = TestPath("free");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 2);

    const auto [page_id, data] = pool.NewPage();
    data[0] = 'x';
    pool.UnpinPage(page_id, true);
    pool.FreePage(page_id);  // must discard the dirty bytes with the page

    const auto [reused_id, reused] = pool.NewPage();
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
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    const auto page_id = pool.NewPage().page_id;  // stays pinned

    EXPECT_DEATH(pool.FreePage(page_id), "freeing a pinned page");

    pool.UnpinPage(page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolDeathTest, UnpinOfNonResidentPageDies) {
  const auto path = TestPath("unpin_missing");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    EXPECT_DEATH(pool.UnpinPage(tinydb::FIRST_DATA_PAGE_ID, false), "not in the pool");
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolDeathTest, DoubleUnpinDies) {
  const auto path = TestPath("double_unpin");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    const auto page_id = pool.NewPage().page_id;
    pool.UnpinPage(page_id, true);

    EXPECT_DEATH(pool.UnpinPage(page_id, false), "unpinning an unpinned page");
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, KeepPinnedPage) {
  const auto path = TestPath("pinned");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    const auto page_id = pool.NewPage().page_id;

    EXPECT_THROW(pool.NewPage(), std::runtime_error);

    pool.UnpinPage(page_id, false);
  }

  std::filesystem::remove(path);
}
