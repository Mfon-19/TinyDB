#include <gtest/gtest.h>
#include <tinydb/disk_manager.h>
#include <tinydb/file_header.h>
#include <tinydb/status.h>

#include <unistd.h>

#include <array>
#include <cstring>
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

    // Metadata (here, next_page_id) reaches the file at a checkpoint, not
    // before; without this the reopen would not know the page exists.
    ASSERT_TRUE(disk.Checkpoint().Ok());
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
    auto disk = tinydb::DiskManager::Open(path).value();
    freed = disk.AllocatePage().value();
    ASSERT_TRUE(disk.AllocatePage().has_value());
    disk.FreePage(freed);
    ASSERT_TRUE(disk.Checkpoint().Ok());  // the free list persists via checkpoints
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

TEST(DiskManagerTest, MetadataWaitsForCheckpoint) {
  const auto path = TestPath("deferred_metadata");
  std::filesystem::remove(path);

  tinydb::page_id_t page_id = 0;
  {
    auto disk = tinydb::DiskManager::Open(path).value();
    page_id = disk.AllocatePage().value();
    // No checkpoint: the allocation only ever lived in memory.
  }

  {
    // The reopened header never heard about the allocation, so the same
    // page comes back. (In the full engine the log replays the header
    // image instead; this layer alone just forgets.)
    auto disk = tinydb::DiskManager::Open(path).value();
    EXPECT_EQ(disk.AllocatePage(), page_id);
  }

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, TakeOpImagesCarriesDeferredMetadata) {
  const auto path = TestPath("op_images");
  std::filesystem::remove(path);

  auto disk = tinydb::DiskManager::Open(path).value();
  const auto page_id = disk.AllocatePage().value();

  // The allocation changed the header: one header image.
  auto images = disk.TakeOpImages();
  ASSERT_EQ(images.size(), 1U);
  EXPECT_EQ(images[0].page_id, tinydb::HEADER_PAGE_ID);
  tinydb::FileHeader header{};
  std::memcpy(&header, images[0].data.data(), sizeof(header));
  EXPECT_EQ(header.next_page_id, page_id + 1);

  // A free adds the freed page's link image alongside the header's.
  disk.FreePage(page_id);
  images = disk.TakeOpImages();
  ASSERT_EQ(images.size(), 2U);
  EXPECT_EQ(images[0].page_id, tinydb::HEADER_PAGE_ID);
  std::memcpy(&header, images[0].data.data(), sizeof(header));
  EXPECT_EQ(header.free_list_head, page_id);
  EXPECT_EQ(images[1].page_id, page_id);
  tinydb::FreePageHeader link{};
  std::memcpy(&link, images[1].data.data(), sizeof(link));
  EXPECT_EQ(link.type, tinydb::FREE_PAGE_TYPE);
  EXPECT_EQ(link.next_free, tinydb::HEADER_PAGE_ID);

  // Taking the images drained the per-op state.
  EXPECT_TRUE(disk.TakeOpImages().empty());

  // Reallocating the freed page drops its pending link: nothing stale is
  // logged for it, only the header change.
  EXPECT_EQ(disk.AllocatePage(), page_id);
  images = disk.TakeOpImages();
  ASSERT_EQ(images.size(), 1U);
  EXPECT_EQ(images[0].page_id, tinydb::HEADER_PAGE_ID);

  std::filesystem::remove(path);
}
