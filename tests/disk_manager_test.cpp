#include <gtest/gtest.h>
#include <tinydb/disk_manager.h>

#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_" + name + "_" + std::to_string(::getpid()) + ".db");
}

TEST(DiskManagerTest, ReopenPage) {
  const auto path = TestPath("write_read_reopen");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    const auto page_id = disk.AllocatePage();
    EXPECT_EQ(page_id, tinydb::FIRST_DATA_PAGE_ID);

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    page[0] = 'a';
    page[tinydb::PAGE_SIZE - 1] = 'z';
    disk.WritePage(page_id, page.data());
  }

  {
    tinydb::DiskManager disk(path);

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, page.data());

    EXPECT_EQ(page[0], 'a');
    EXPECT_EQ(page[tinydb::PAGE_SIZE - 1], 'z');
    EXPECT_EQ(disk.AllocatePage(), tinydb::FIRST_DATA_PAGE_ID + 1);
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, FreedPagesAreReusedNewestFirst) {
  const auto path = TestPath("free_reuse");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    const auto first = disk.AllocatePage();
    static_cast<void>(disk.AllocatePage());
    const auto third = disk.AllocatePage();
    const auto size_before = std::filesystem::file_size(path);

    disk.FreePage(first);
    disk.FreePage(third);

    // LIFO: the most recently freed page comes back first, without growth.
    EXPECT_EQ(disk.AllocatePage(), third);
    EXPECT_EQ(disk.AllocatePage(), first);
    EXPECT_EQ(std::filesystem::file_size(path), size_before);

    // With the free list drained, the file grows again.
    EXPECT_EQ(disk.AllocatePage(), third + 1);
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, FreeListSurvivesReopen) {
  const auto path = TestPath("free_reopen");
  std::filesystem::remove(path);

  tinydb::page_id_t freed = 0;
  {
    tinydb::DiskManager disk(path);
    freed = disk.AllocatePage();
    static_cast<void>(disk.AllocatePage());
    disk.FreePage(freed);
  }

  {
    tinydb::DiskManager disk(path);
    EXPECT_EQ(disk.AllocatePage(), freed);
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerDeathTest, DoubleFreeDies) {
  const auto path = TestPath("double_free");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    const auto page_id = disk.AllocatePage();
    disk.FreePage(page_id);

    EXPECT_DEATH(disk.FreePage(page_id), "double free");
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsTruncatedFile) {
  const auto path = TestPath("truncated");
  std::filesystem::remove(path);

  {
    auto file = std::ofstream{path};
    file << "abc";  // non-empty, but shorter than a file header
  }

  EXPECT_THROW(tinydb::DiskManager{path}, std::runtime_error);

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, SyncAfterWrites) {
  const auto path = TestPath("sync");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    const auto page_id = disk.AllocatePage();

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    page[0] = 's';
    disk.WritePage(page_id, page.data());
    disk.Sync();  // must not throw on a healthy descriptor
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, UnallocatedRead) {
  const auto path = TestPath("unallocated_read");
  std::filesystem::remove(path);

  tinydb::DiskManager disk(path);
  auto page = std::array<char, tinydb::PAGE_SIZE>{};

  EXPECT_THROW(disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, page.data()), std::out_of_range);

  std::filesystem::remove(path);
}
