#include <gtest/gtest.h>
#include <tinydb/disk_manager.h>
#include <tinydb/status.h>

#include "io/syscalls.h"
#include "storage/superblock.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_" + name + "_" + std::to_string(::getpid()) + ".db");
}

auto ParentPath(const std::filesystem::path &path) -> std::filesystem::path {
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

auto FindCall(const std::vector<tinydb::io::Call> &calls, tinydb::io::Syscall syscall,
              const std::filesystem::path &path, std::size_t start = 0) -> std::size_t {
  for (auto index = start; index < calls.size(); ++index) {
    if (calls[index].syscall == syscall && calls[index].path == path) {
      return index;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

void FlipByteAt(const std::filesystem::path &path, std::uint64_t offset) {
  auto file = std::fstream(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.get(byte);
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(byte ^ 0x1));
}

void AdoptOnePage(tinydb::DiskManager &disk, tinydb::page_id_t root, std::uint64_t transaction_id = 1) {
  const auto high_water = tinydb::FIRST_DATA_PAGE_ID + 1;
  disk.AdoptState(root, tinydb::HEADER_PAGE_ID, high_water, transaction_id, 0);
  ASSERT_TRUE(disk.EnsurePageCount(high_water).Ok());
}

}  // namespace

TEST(DiskManagerTest, PreparedStateDoesNotMutatePublishedMetadata) {
  const auto path = TestPath("prepare_state");
  std::filesystem::remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();

  const auto image = disk.PrepareStateImage(tinydb::FIRST_DATA_PAGE_ID, 0, tinydb::FIRST_DATA_PAGE_ID + 1, 1, 0);
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(disk.GetRootPageId(), tinydb::HEADER_PAGE_ID);
  EXPECT_EQ(disk.HighWaterPageId(), tinydb::FIRST_DATA_PAGE_ID);

  const auto state = tinydb::storage::DecodeSuperblock(std::as_bytes(std::span{image->data}));
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->root_page_id, tinydb::FIRST_DATA_PAGE_ID);
  EXPECT_EQ(state->high_water_page_id, tinydb::FIRST_DATA_PAGE_ID + 1);
  EXPECT_EQ(state->transaction_id, 1U);
  std::filesystem::remove(path);
}

TEST(DiskManagerTest, AdoptCheckpointAndReopenPage) {
  const auto path = TestPath("write_read_reopen");
  std::filesystem::remove(path);
  {
    auto disk = tinydb::DiskManager::Open(path).value();
    AdoptOnePage(disk, tinydb::FIRST_DATA_PAGE_ID);
    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    page.front() = 'a';
    page.back() = 'z';
    ASSERT_TRUE(disk.WritePage(tinydb::FIRST_DATA_PAGE_ID, page.data()).Ok());
    disk.AdvanceCheckpoint(1);
    ASSERT_TRUE(disk.Checkpoint().Ok());
    ASSERT_TRUE(disk.Sync().Ok());
  }
  {
    auto disk = tinydb::DiskManager::Open(path).value();
    EXPECT_EQ(disk.GetRootPageId(), tinydb::FIRST_DATA_PAGE_ID);
    EXPECT_EQ(disk.HighWaterPageId(), tinydb::FIRST_DATA_PAGE_ID + 1);
    EXPECT_EQ(disk.CheckpointLsn(), 1U);
    auto page = std::array<char, tinydb::PAGE_SIZE>{};
    ASSERT_TRUE(disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, page.data()).Ok());
    EXPECT_EQ(page.front(), 'a');
    EXPECT_EQ(page.back(), 'z');
  }
  std::filesystem::remove(path);
}

TEST(DiskManagerTest, FileGrowthDoesNotAllocateLogicalPages) {
  const auto path = TestPath("physical_growth");
  std::filesystem::remove(path);
  auto disk = tinydb::DiskManager::Open(path).value();
  ASSERT_TRUE(disk.EnsurePageCount(tinydb::FIRST_DATA_PAGE_ID + 10).Ok());
  EXPECT_EQ(disk.HighWaterPageId(), tinydb::FIRST_DATA_PAGE_ID);
  auto bytes = std::array<char, tinydb::PAGE_SIZE>{};
  EXPECT_EQ(disk.ReadPage(tinydb::FIRST_DATA_PAGE_ID, bytes.data()).Code(), tinydb::StatusCode::InvalidArgument);
  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsTruncatedAndOldFormatsWithoutMutation) {
  for (const auto &[name, bytes] :
       std::array{std::pair{std::string{"truncated"}, std::string{"abc"}},
                  std::pair{std::string{"old"}, std::string(2 * tinydb::PAGE_SIZE, '\0')}}) {
    const auto path = TestPath(name);
    std::filesystem::remove(path);
    auto contents = bytes;
    if (name == "old") {
      contents.replace(0, 4, "TDB3");
    }
    auto file = std::ofstream(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    const auto before = std::filesystem::file_size(path);
    const auto result = tinydb::DiskManager::Open(path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().Code(), tinydb::StatusCode::UnsupportedFormat);
    EXPECT_EQ(std::filesystem::file_size(path), before);
    std::filesystem::remove(path);
  }
}

TEST(DiskManagerTest, OneValidSuperblockSurvivesDamageToTheOther) {
  const auto path = TestPath("one_superblock");
  std::filesystem::remove(path);
  {
    auto disk = tinydb::DiskManager::Open(path).value();
    AdoptOnePage(disk, tinydb::FIRST_DATA_PAGE_ID);
    disk.AdvanceCheckpoint(1);
    ASSERT_TRUE(disk.Checkpoint().Ok());
    ASSERT_TRUE(disk.Sync().Ok());
  }
  FlipByteAt(path, tinydb::storage::superblock_offset::GENERATION);
  const auto reopened = tinydb::DiskManager::Open(path);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->HighWaterPageId(), tinydb::FIRST_DATA_PAGE_ID + 1);
  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsTwoCorruptSuperblocks) {
  const auto path = TestPath("corrupt_headers");
  std::filesystem::remove(path);
  ASSERT_TRUE(tinydb::DiskManager::Open(path).has_value());
  FlipByteAt(path, tinydb::storage::superblock_offset::GENERATION);
  FlipByteAt(path, tinydb::PAGE_SIZE + tinydb::storage::superblock_offset::GENERATION);
  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::Corruption);
  std::filesystem::remove(path);
}

TEST(DiskManagerTest, RejectsAFrontierBeyondTheFile) {
  const auto path = TestPath("short_file");
  std::filesystem::remove(path);
  {
    auto disk = tinydb::DiskManager::Open(path).value();
    AdoptOnePage(disk, tinydb::FIRST_DATA_PAGE_ID);
    disk.AdvanceCheckpoint(1);
    ASSERT_TRUE(disk.Checkpoint().Ok());
    ASSERT_TRUE(disk.Sync().Ok());
  }
  std::filesystem::resize_file(path, 2 * tinydb::PAGE_SIZE);
  const auto result = tinydb::DiskManager::Open(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::Corruption);
  std::filesystem::remove(path);
}

TEST(DiskManagerDurabilityTest, OpenOrdersFileAndDirectorySyncs) {
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
