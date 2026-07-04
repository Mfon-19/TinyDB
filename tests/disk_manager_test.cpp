#include <gtest/gtest.h>
#include <tinydb/disk_manager.h>
#include <tinydb/status.h>

#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_" + name + "_" + std::to_string(::getpid()) + ".db");
}

TEST(DiskManagerTest, ReopenPage) {
  const auto path = TestPath("write_read_reopen");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    const auto page_id = disk.AllocatePage().value();
    EXPECT_EQ(page_id, tinydb::FIRST_DATA_PAGE_ID);

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    page[0] = 'a';
    page[tinydb::PAGE_SIZE - 1] = 'z';
    ASSERT_TRUE(disk.WritePage(page_id, page.data()).Ok());
  }

  {
    auto disk = tinydb::DiskManager::Open(path).value();

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    ASSERT_TRUE(disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, page.data()).Ok());

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
    auto disk = tinydb::DiskManager::Open(path).value();
    const auto first = disk.AllocatePage().value();
    ASSERT_TRUE(disk.AllocatePage().has_value());
    const auto third = disk.AllocatePage().value();
    const auto size_before = std::filesystem::file_size(path);

    ASSERT_TRUE(disk.FreePage(first).Ok());
    ASSERT_TRUE(disk.FreePage(third).Ok());

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
    auto disk = tinydb::DiskManager::Open(path).value();
    freed = disk.AllocatePage().value();
    ASSERT_TRUE(disk.AllocatePage().has_value());
    ASSERT_TRUE(disk.FreePage(freed).Ok());
  }

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    EXPECT_EQ(disk.AllocatePage(), freed);
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerDeathTest, DoubleFreeDies) {
  const auto path = TestPath("double_free");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    const auto page_id = disk.AllocatePage().value();
    ASSERT_TRUE(disk.FreePage(page_id).Ok());

    EXPECT_DEATH(static_cast<void>(disk.FreePage(page_id)), "double free");
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

  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::InvalidArgument);

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, SyncAfterWrites) {
  const auto path = TestPath("sync");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    const auto page_id = disk.AllocatePage().value();

    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    page[0] = 's';
    ASSERT_TRUE(disk.WritePage(page_id, page.data()).Ok());
    EXPECT_TRUE(disk.Sync().Ok());  // must succeed on a healthy descriptor
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, UnallocatedRead) {
  const auto path = TestPath("unallocated_read");
  std::filesystem::remove(path);

  auto disk = tinydb::DiskManager::Open(path).value();
  auto page = std::array<char, tinydb::PAGE_SIZE>{};

  const auto status = disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, page.data());
  EXPECT_EQ(status.Code(), tinydb::StatusCode::InvalidArgument);

  std::filesystem::remove(path);
}
