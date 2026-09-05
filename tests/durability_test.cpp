#include "tinydb/database.h"
#include "tinydb/storage/page_codec.h"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iterator>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

enum class Operation { None, Write };

struct Fault {
  Operation operation = Operation::None;
  ino_t inode = 0;
  dev_t device = 0;
  int skip = 0;
};

thread_local Fault fault;
thread_local int mutations = 0;
thread_local bool fail_after_sync = false;
thread_local bool fail_allocation = false;

bool Fail(int fd, Operation operation) {
  ++mutations;
  if (fault.operation != operation) {
    return false;
  }
  struct stat info {};
  if (fstat(fd, &info) != 0 || info.st_ino != fault.inode ||
      info.st_dev != fault.device) {
    return false;
  }
  if (fault.skip-- > 0) {
    return false;
  }
  fault.operation = Operation::None;
  return true;
}

void Arm(const std::string &path, Operation operation, int skip = 0) {
  struct stat info {};
  ASSERT_EQ(stat(path.c_str(), &info), 0);
  fault = {operation, info.st_ino, info.st_dev, skip};
}

}

extern "C" ssize_t __real_pwrite(int, const void *, size_t, off_t);

extern "C" ssize_t __wrap_pwrite(int fd, const void *bytes, size_t size,
                                 off_t offset) {
  if (!Fail(fd, Operation::Write)) {
    return __real_pwrite(fd, bytes, size, offset);
  }
  errno = EIO;
  return -1;
}

namespace tinydb {
namespace {

auto Bytes(const std::string &path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{file}, {}};
}

auto Page(storage::PageId page_id,
          std::string_view value) -> storage::PageBytes {
  const storage::LeafEntry entry{"key", value};
  return storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                                 std::span{&entry, 1})
      .value();
}

class DurabilityTest : public testing::Test {
protected:
  void SetUp() override {
    fault = {};
    fail_after_sync = fail_allocation = false;
    directory_ = testing::TempDir() + "tinydb_durable_XXXXXX";
    ASSERT_NE(mkdtemp(directory_.data()), nullptr);
    path_ = directory_ + "/database";
  }

  void TearDown() override {
    fault = {};
    fail_after_sync = fail_allocation = false;
    std::filesystem::remove_all(directory_);
  }

  std::string directory_;
  std::string path_;
};

TEST_F(DurabilityTest, DirtyPagesStayResident) {
  auto disk = storage::DiskManager::Open(path_).value();
  for (storage::PageId id = 1; id <= 3; ++id) {
    ASSERT_TRUE(disk.WritePage(id, Page(id, "old")).Ok());
  }
  cache::BufferPool pool(std::move(disk), 2);
  ASSERT_TRUE(pool.InstallPage(1, Page(1, "dirty")).Ok());
  {
    auto pinned = pool.ReadPage(2).value();
    EXPECT_FALSE(pool.ReadPage(3));
    EXPECT_EQ(pool.ReadPage(1).value().Bytes(), Page(1, "dirty"));
  }
  EXPECT_TRUE(pool.ReadPage(3));
  EXPECT_EQ(pool.ReadPage(1).value().Bytes(), Page(1, "dirty"));
  const storage::WalPages incoming{{1, Page(1, "checkpointed")}};
  ASSERT_TRUE(pool.Flush(incoming).Ok());
  EXPECT_EQ(pool.ReadPage(1).value().Bytes(), incoming.at(1));
  auto pinned = pool.ReadPage(2).value();
  EXPECT_TRUE(pool.ReadPage(3));
}

TEST_F(DurabilityTest, CheckpointPressure) {
  auto database = Database::Open(path_, 4).value();
  const auto original = Bytes(path_);
  ASSERT_TRUE(database->Put("key", "one").Ok());
  ASSERT_TRUE(database->Put("key", "two").Ok());
  EXPECT_EQ(Bytes(path_), original);
  EXPECT_EQ(std::filesystem::file_size(path_ + "-wal"), 2 * 4112U);
  EXPECT_EQ(database->Get("key").value(), "two");
  ASSERT_TRUE(database->Put("key", "three").Ok());
  EXPECT_TRUE(Bytes(path_ + "-wal").empty());
  EXPECT_NE(Bytes(path_), original);
  EXPECT_EQ(database->Get("key").value(), "three");
  ASSERT_TRUE(database->Put("other", "pending").Ok());
  ASSERT_TRUE(database->Checkpoint().Ok());
  EXPECT_TRUE(Bytes(path_ + "-wal").empty());
  EXPECT_EQ(database->Get("other").value(), "pending");

  auto transaction = database->BeginWrite().value();
  const std::string value(900, 'v');
  for (int index = 0; index < 80; ++index) {
    ASSERT_TRUE(transaction->Put(std::format("k{:03}", index), value).Ok());
  }
  ASSERT_TRUE(transaction->Commit().Ok());
  EXPECT_TRUE(Bytes(path_ + "-wal").empty());
  EXPECT_GT(std::filesystem::file_size(path_), 4 * storage::PAGE_SIZE);
  EXPECT_EQ(database->Get("k079").value(), value);
  const int calls = mutations;
  ASSERT_TRUE(database->BeginWrite().value()->Commit().Ok());
  EXPECT_EQ(mutations, calls);
  transaction.reset();
  database.reset();
  database = Database::Open(path_, 4).value();
  EXPECT_EQ(database->Get("k079").value(), value);
}

TEST_F(DurabilityTest, CommitSurvivesExit) {
  Database::Open(path_, 64).value().reset();
  const auto original = Bytes(path_);
  ASSERT_EXIT(
      {
        auto database = Database::Open(path_, 64).value();
        if (!database->Put("a", "old").Ok() ||
            !database->Put("b", "kept").Ok() ||
            !database->Delete("a").value()) {
          _exit(1);
        }
        _exit(0);
      },
      testing::ExitedWithCode(0), "");
  EXPECT_EQ(Bytes(path_), original);
  EXPECT_EQ(std::filesystem::file_size(path_ + "-wal"), 3 * 4112U);
  {
    std::ofstream file(path_ + "-wal", std::ios::binary | std::ios::app);
    file << "TDW";
  }
  auto database = Database::Open(path_, 64).value();
  EXPECT_EQ(database->Get("a").value(), std::nullopt);
  EXPECT_EQ(database->Get("b").value(), "kept");
  EXPECT_TRUE(Bytes(path_ + "-wal").empty());
}

TEST_F(DurabilityTest, RejectsCorruptWal) {
  {
    auto database = Database::Open(path_, 8).value();
    ASSERT_TRUE(database->Put("key", "committed").Ok());
  }
  auto bad = storage::EncodeWalRecord({{1, Page(1, "bad")}}).value();
  bad[0] = 'X';
  {
    std::ofstream file(path_ + "-wal", std::ios::binary | std::ios::app);
    file.write(bad.data(), static_cast<std::streamsize>(bad.size()));
  }
  const auto database_bytes = Bytes(path_);
  const auto wal_bytes = Bytes(path_ + "-wal");
  EXPECT_FALSE(Database::Open(path_, 8));
  EXPECT_EQ(Bytes(path_), database_bytes);
  EXPECT_EQ(Bytes(path_ + "-wal"), wal_bytes);
  std::ofstream(path_, std::ios::trunc).close();
  EXPECT_FALSE(Database::Open(path_, 8));
  EXPECT_TRUE(Bytes(path_).empty());
  EXPECT_EQ(Bytes(path_ + "-wal"), wal_bytes);
}

TEST_F(DurabilityTest, RecoveryCanRetry) {
  const std::string value(900, 'v');
  {
    auto database = Database::Open(path_, 256).value();
    auto transaction = database->BeginWrite().value();
    for (int index = 0; index < 80; ++index) {
      ASSERT_TRUE(transaction->Put(std::format("k{:03}", index), value).Ok());
    }
    ASSERT_TRUE(transaction->Commit().Ok());
  }
  const auto database_bytes = Bytes(path_);
  const auto wal_bytes = Bytes(path_ + "-wal");
  ASSERT_FALSE(wal_bytes.empty());
  Arm(path_, Operation::Write, 1);
  EXPECT_FALSE(Database::Open(path_, 8));
  EXPECT_NE(Bytes(path_), database_bytes);
  EXPECT_EQ(Bytes(path_ + "-wal"), wal_bytes);
  fault = {};
  auto database = Database::Open(path_, 8).value();
  for (int index = 0; index < 80; ++index) {
    EXPECT_EQ(database->Get(std::format("k{:03}", index)).value(), value);
  }
  EXPECT_TRUE(Bytes(path_ + "-wal").empty());
}

TEST_F(DurabilityTest, CheckpointWaitsForWriter) {
  using namespace std::chrono_literals;
  auto database = Database::Open(path_, 8).value();
  std::promise<void> started;
  auto ready = started.get_future();
  std::jthread worker;
  auto transaction = database->BeginWrite().value();
  ASSERT_TRUE(transaction->Put("key", "value").Ok());
  std::packaged_task<Status()> checkpoint([&] {
    started.set_value();
    return database->Checkpoint();
  });
  auto result = checkpoint.get_future();
  worker = std::jthread(std::move(checkpoint));
  ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(result.wait_for(50ms), std::future_status::timeout);
  ASSERT_TRUE(transaction->Commit().Ok());
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(result.get().Ok());
  EXPECT_TRUE(Bytes(path_ + "-wal").empty());
  EXPECT_EQ(database->Get("key").value(), "value");
}

}
}
