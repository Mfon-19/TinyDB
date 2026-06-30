#include <gtest/gtest.h>
#include <tinydb/buffer_pool.h>

#include <unistd.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tinydb_buffer_" + name + "_" + std::to_string(::getpid()) + ".db");
}

TEST(BufferPoolTest, FlushNewPage) {
  const auto path = TestPath("flush");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    tinydb::page_id_t page_id = 0;
    char *data = pool.NewPage(&page_id);
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

    tinydb::page_id_t first_page_id = 0;
    char *first_page = pool.NewPage(&first_page_id);
    first_page[0] = 'a';
    pool.UnpinPage(first_page_id, true);

    tinydb::page_id_t second_page_id = 0;
    char *second_page = pool.NewPage(&second_page_id);
    second_page[0] = 'z';
    pool.UnpinPage(second_page_id, true);

    char *fetched_page = pool.FetchPage(first_page_id);
    EXPECT_EQ(fetched_page[0], 'a');
    pool.UnpinPage(first_page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BufferPoolTest, KeepPinnedPage) {
  const auto path = TestPath("pinned");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool pool(&disk, 1);

    tinydb::page_id_t page_id = 0;
    pool.NewPage(&page_id);

    tinydb::page_id_t blocked_page_id = 0;
    EXPECT_THROW(pool.NewPage(&blocked_page_id), std::runtime_error);

    pool.UnpinPage(page_id, false);
  }

  std::filesystem::remove(path);
}
