#include <gtest/gtest.h>
#include <tinydb/disk_manager.h>
#include <tinydb/status.h>

#include "io/syscalls.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_" + name + "_" + std::to_string(::getpid()) + ".db");
}

static auto ParentPath(const std::filesystem::path &path) -> std::filesystem::path {
  const auto parent = path.parent_path();
  return parent.empty() ? std::filesystem::path{"."} : parent;
}

class ScopedSyscallHook {
 public:
  explicit ScopedSyscallHook(tinydb::io::TestHook hook) { tinydb::io::SetTestHook(std::move(hook)); }
  ScopedSyscallHook(const ScopedSyscallHook &) = delete;
  auto operator=(const ScopedSyscallHook &) -> ScopedSyscallHook & = delete;
  ~ScopedSyscallHook() { tinydb::io::ClearTestHook(); }
};

static auto FindCall(const std::vector<tinydb::io::Call> &calls, tinydb::io::Syscall syscall,
                     const std::filesystem::path &path, std::size_t start = 0) -> std::size_t {
  for (auto i = start; i < calls.size(); ++i) {
    if (calls[i].syscall == syscall && calls[i].path == path) {
      return i;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

static void FlipByteAt(const std::filesystem::path &path, std::uint64_t offset) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.get(byte);
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(byte ^ 0x1));
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
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::UnsupportedFormat);

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsTheOldSingleHeaderFormatWithoutMutation) {
  const auto path = TestPath("old_format");
  std::filesystem::remove(path);
  {
    auto bytes = std::string(2 * tinydb::PAGE_SIZE, '\0');
    bytes.replace(0, 4, "TDB3");
    auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  const auto before = std::filesystem::file_size(path);

  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::UnsupportedFormat);
  EXPECT_EQ(std::filesystem::file_size(path), before);

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsACorruptedHeader) {
  const auto path = TestPath("corrupt_header");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    ASSERT_TRUE(disk.AllocatePage().has_value());
    ASSERT_TRUE(disk.Checkpoint().Ok());
  }

  // Both copies must be damaged before the database loses its recoverable
  // metadata state; either superblock alone is sufficient.
  FlipByteAt(path, tinydb::storage::superblock_offset::GENERATION);
  FlipByteAt(path, tinydb::PAGE_SIZE + tinydb::storage::superblock_offset::GENERATION);

  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::Corruption);

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, OneValidSuperblockSurvivesDamageToTheOther) {
  const auto path = TestPath("one_superblock");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    ASSERT_TRUE(disk.AllocatePage().has_value());
    ASSERT_TRUE(disk.Checkpoint().Ok());
    ASSERT_TRUE(disk.Sync().Ok());
  }

  FlipByteAt(path, tinydb::storage::superblock_offset::GENERATION);
  const auto reopened = tinydb::DiskManager::Open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->NextPageId(), tinydb::FIRST_DATA_PAGE_ID + 1);

  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsAHeaderThatClaimsPagesBeyondTheFile) {
  const auto path = TestPath("short_file");
  std::filesystem::remove(path);

  {
    auto disk = tinydb::DiskManager::Open(path).value();
    ASSERT_TRUE(disk.AllocatePage().has_value());
    ASSERT_TRUE(disk.Checkpoint().Ok());
  }

  // Chop off the allocated page: the header is intact but now promises a
  // page the file no longer holds.
  std::filesystem::resize_file(path, 2 * tinydb::PAGE_SIZE);

  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::Corruption);

  std::filesystem::remove(path);
}

TEST(DiskManagerDurabilityTest, OpenMakesAFreshHeaderDurable) {
  const auto path = TestPath("fresh_header_durable");
  std::filesystem::remove(path);

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(tinydb::DiskManager::Open(path).has_value());
  }

  const auto first_write = FindCall(calls, tinydb::io::Syscall::Pwrite, path);
  const auto first_sync = FindCall(calls, tinydb::io::Syscall::Fsync, path, first_write + 1);
  const auto parent_sync = FindCall(calls, tinydb::io::Syscall::Fsync, ParentPath(path), first_sync + 1);
  const auto second_write = FindCall(calls, tinydb::io::Syscall::Pwrite, path, parent_sync + 1);
  const auto second_sync = FindCall(calls, tinydb::io::Syscall::Fsync, path, second_write + 1);
  ASSERT_NE(first_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(first_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(parent_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(second_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(second_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(first_write, first_sync);
  EXPECT_LT(first_sync, parent_sync);
  EXPECT_LT(parent_sync, second_write);
  EXPECT_LT(second_write, second_sync);

  std::filesystem::remove(path);
}

TEST(DiskManagerDurabilityTest, OpenFailsWhenCreationDirectorySyncFails) {
  const auto path = TestPath("fresh_parent_sync_fails");
  std::filesystem::remove(path);
  auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Fsync && call.path == ParentPath(path)) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  }};

  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::IoError);

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
  EXPECT_EQ(images[0].page_id, tinydb::SUPERBLOCK_B_PAGE_ID);
  auto header = tinydb::storage::DecodeSuperblock(std::as_bytes(std::span{images[0].data}));
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->high_water_page_id, page_id + 1);

  // A free adds the freed page's link image alongside the header's.
  disk.FreePage(page_id);
  images = disk.TakeOpImages();
  ASSERT_EQ(images.size(), 2U);
  EXPECT_EQ(images[0].page_id, tinydb::SUPERBLOCK_A_PAGE_ID);
  header = tinydb::storage::DecodeSuperblock(std::as_bytes(std::span{images[0].data}));
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->allocator_root_page_id, page_id);
  EXPECT_EQ(images[1].page_id, page_id);
  const auto link = tinydb::storage::DecodeAllocatorPage(std::as_bytes(std::span{images[1].data}), page_id);
  ASSERT_TRUE(link.has_value());
  EXPECT_EQ(link->next_free, tinydb::HEADER_PAGE_ID);

  // Taking the images drained the per-op state.
  EXPECT_TRUE(disk.TakeOpImages().empty());

  // Reallocating the freed page drops its pending link: nothing stale is
  // logged for it, only the header change.
  EXPECT_EQ(disk.AllocatePage(), page_id);
  images = disk.TakeOpImages();
  ASSERT_EQ(images.size(), 1U);
  EXPECT_EQ(images[0].page_id, tinydb::SUPERBLOCK_B_PAGE_ID);

  std::filesystem::remove(path);
}
