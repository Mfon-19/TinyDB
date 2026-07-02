#include <gtest/gtest.h>
#include <tinydb/disk_manager.h>

#include <unistd.h>

#include <array>
#include <filesystem>
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

TEST(DiskManagerTest, UnallocatedRead) {
  const auto path = TestPath("unallocated_read");
  std::filesystem::remove(path);

  tinydb::DiskManager disk(path);
  auto page = std::array<char, tinydb::PAGE_SIZE>{};

  EXPECT_THROW(disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, page.data()), std::out_of_range);

  std::filesystem::remove(path);
}
