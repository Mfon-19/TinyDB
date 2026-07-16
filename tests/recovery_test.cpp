#include <gtest/gtest.h>
#include "storage/database_uuid.h"
#include "storage/page.h"
#include "wal/wal.h"

#include "io/syscalls.h"
#include "recovery/recovery.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "txn/database_state.h"
#include "wal/wal_codec.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/*
** RECOVERY PROTOCOL TESTS
**
** These tests operate below Database so they can isolate the recovery
** state machine.  A fixture contains a checkpointed database plus the exact
** WAL suffix to recover.  Each crash iteration copies that same fixture,
** terminates a child immediately before one hooked filesystem call, and then
** runs recovery again in the parent.
**
** The required outcome is independent of the crash point:
**
**   old superblock + complete WAL  -> repeat physical redo
**   new superblock + stale WAL     -> skip covered redo and finish cleanup
**
** No destructor, Close, or error-unwind path runs in the killed child.  Thus
** the sweep exercises the same persistent intermediate states a process crash
** can expose, while the WAL ordering tests separately establish which writes
** must be synchronized for power-loss durability.
*/

constexpr std::uint64_t WAL_HEADER_BYTES = tinydb::wal_format::HEADER_BYTES;
constexpr std::uint64_t PAGE_IMAGE_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + tinydb::wal_format::PAGE_IMAGE_PAYLOAD_BYTES;
constexpr std::uint64_t DATABASE_STATE_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + tinydb::wal_format::DATABASE_STATE_PAYLOAD_BYTES;
constexpr std::uint64_t COMMIT_RECORD_BYTES =
    tinydb::wal_format::RECORD_HEADER_BYTES + tinydb::wal_format::COMMIT_PAYLOAD_BYTES;
constexpr std::uint64_t ONE_PAGE_TRANSACTION_BYTES =
    PAGE_IMAGE_RECORD_BYTES + DATABASE_STATE_RECORD_BYTES + COMMIT_RECORD_BYTES;

auto Uuid(std::byte last) -> tinydb::DatabaseUuid {
  auto uuid = tinydb::DatabaseUuid{};
  uuid.back() = last;
  return uuid;
}

const auto TEST_UUID = Uuid(std::byte{0x71});

auto TestPath(std::string_view name, std::size_t instance = 0) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_recovery_" + std::string{name} + "_" +
                                                   std::to_string(::getpid()) + "_" + std::to_string(instance) + ".db");
}

auto WalPath(const std::filesystem::path &db_path) -> std::filesystem::path { return tinydb::Wal::PathFor(db_path); }

void RemoveFixture(const std::filesystem::path &db_path) {
  const auto wal_path = WalPath(db_path);
  std::error_code ignored;
  std::filesystem::remove(db_path, ignored);
  std::filesystem::remove(wal_path, ignored);
  for (std::uint64_t segment_id = 1; segment_id <= 8; ++segment_id) {
    std::filesystem::remove(tinydb::Wal::SegmentPathFor(wal_path, segment_id), ignored);
  }
}

class RecoveryTest : public ::testing::Test {
 protected:
  auto NewPath(std::string_view name, std::size_t instance = 0) -> std::filesystem::path {
    auto path = TestPath(name, instance);
    RemoveFixture(path);
    paths_.push_back(path);
    return path;
  }

  void TearDown() override {
    tinydb::io::ClearTestHook();
    for (const auto &path : paths_) {
      RemoveFixture(path);
    }
  }

 private:
  std::vector<std::filesystem::path> paths_;
};

auto DataPage(tinydb::page_id_t page_id, char marker) -> std::array<char, tinydb::PAGE_SIZE> {
  const auto payload = std::array{static_cast<std::byte>(marker)};
  auto encoded = tinydb::storage::EncodeOverflowPage(page_id, 0, page_id, 0, tinydb::HEADER_PAGE_ID, payload);
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::array<char, tinydb::PAGE_SIZE>{});
}

void MakeDatabase(const std::filesystem::path &db_path, tinydb::page_id_t page_count = 4) {
  auto output = std::ofstream(db_path, std::ios::binary | std::ios::trunc);
  auto superblock = tinydb::storage::EncodeSuperblock(tinydb::storage::Superblock{
      .database_uuid = TEST_UUID,
      .generation = 1,
      .checkpoint_lsn = 0,
      .transaction_id = 0,
      .root_page_id = tinydb::HEADER_PAGE_ID,
      .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
      .high_water_page_id = page_count,
  });
  ASSERT_TRUE(superblock.has_value());
  output.write(reinterpret_cast<const char *>(superblock->data()), static_cast<std::streamsize>(superblock->size()));
  output.write(reinterpret_cast<const char *>(superblock->data()), static_cast<std::streamsize>(superblock->size()));
  for (auto page_id = tinydb::FIRST_DATA_PAGE_ID; page_id < page_count; ++page_id) {
    const auto page = DataPage(page_id, 'x');
    output.write(page.data(), static_cast<std::streamsize>(page.size()));
  }
  ASSERT_TRUE(output.good());
}

auto State(tinydb::page_id_t high_water = 4) -> tinydb::txn::DatabaseState {
  return tinydb::txn::DatabaseState{
      .root_page_id = tinydb::HEADER_PAGE_ID,
      .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
      .high_water_page_id = high_water,
      .transaction_id = 0,
      .visible_lsn = 0,
      .checkpoint_lsn = 0,
  };
}

void MakeRotatedWal(const std::filesystem::path &db_path) {
  const auto wal_path = WalPath(db_path);
  auto wal = tinydb::Wal::Open(wal_path, TEST_UUID, 1, 0, WAL_HEADER_BYTES + ONE_PAGE_TRANSACTION_BYTES);
  ASSERT_TRUE(wal.has_value());
  const auto first = DataPage(2, 'a');
  wal->AppendPageImage(2, first.data());
  ASSERT_TRUE(wal->Commit(State()).has_value());
  const auto second = DataPage(3, 'b');
  wal->AppendPageImage(3, second.data());
  ASSERT_TRUE(wal->Commit(State()).has_value());
  ASSERT_TRUE(std::filesystem::exists(tinydb::Wal::SegmentPathFor(wal_path, 1)));
}

void MakeLinearWal(const std::filesystem::path &db_path, std::size_t transaction_count) {
  auto wal = tinydb::Wal::Open(WalPath(db_path), TEST_UUID);
  ASSERT_TRUE(wal.has_value());
  for (std::size_t index = 0; index < transaction_count; ++index) {
    const auto page_id = static_cast<tinydb::page_id_t>(2 + (index % 2));
    const auto page = DataPage(page_id, static_cast<char>('a' + index));
    wal->AppendPageImage(page_id, page.data());
    ASSERT_TRUE(wal->Commit(State()).has_value());
  }
}

void CopyFixture(const std::filesystem::path &source_db, const std::filesystem::path &target_db) {
  std::filesystem::copy_file(source_db, target_db, std::filesystem::copy_options::overwrite_existing);
  const auto source_wal = WalPath(source_db);
  const auto target_wal = WalPath(target_db);
  std::filesystem::copy_file(source_wal, target_wal, std::filesystem::copy_options::overwrite_existing);
  for (std::uint64_t segment_id = 1; segment_id <= 8; ++segment_id) {
    const auto source_segment = tinydb::Wal::SegmentPathFor(source_wal, segment_id);
    if (std::filesystem::exists(source_segment)) {
      std::filesystem::copy_file(source_segment, tinydb::Wal::SegmentPathFor(target_wal, segment_id),
                                 std::filesystem::copy_options::overwrite_existing);
    }
  }
}

auto ReadFile(const std::filesystem::path &path) -> std::string {
  auto input = std::ifstream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void FlipByte(const std::filesystem::path &path, std::uint64_t offset) {
  auto file = std::fstream(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.get(byte);
  file.seekp(static_cast<std::streamoff>(offset));
  file.put(static_cast<char>(byte ^ 0x1));
  ASSERT_TRUE(file.good());
}

void ExpectRecovered(const std::filesystem::path &db_path) {
  const auto database = ReadFile(db_path);
  ASSERT_EQ(database.size(), 4 * tinydb::PAGE_SIZE);
  const auto page_a = std::as_bytes(std::span{database.data(), tinydb::PAGE_SIZE});
  const auto page_b = std::as_bytes(std::span{database.data() + tinydb::PAGE_SIZE, tinydb::PAGE_SIZE});
  const auto selected = tinydb::storage::SelectSuperblock(page_a, page_b);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->value.generation, 2U);
  EXPECT_EQ(selected->value.transaction_id, 2U);
  EXPECT_EQ(selected->value.checkpoint_lsn, 6U);
  EXPECT_EQ(database.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(DataPage(2, 'a').data(), tinydb::PAGE_SIZE));
  EXPECT_EQ(database.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE),
            std::string(DataPage(3, 'b').data(), tinydb::PAGE_SIZE));

  const auto wal_path = WalPath(db_path);
  const auto active = ReadFile(wal_path);
  ASSERT_EQ(active.size(), WAL_HEADER_BYTES);
  const auto header = tinydb::wal_format::DecodeHeader(std::as_bytes(std::span{active}));
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->starting_lsn, 7U);
  EXPECT_FALSE(std::filesystem::exists(tinydb::Wal::SegmentPathFor(wal_path, 1)));
}

TEST_F(RecoveryTest, RestartAtEveryFilesystemBoundaryReachesTheSameDurableState) {
  const auto source = NewPath("crash_source");
  MakeDatabase(source);
  MakeRotatedWal(source);

  // A successful copy establishes the exact number and order of calls in the
  // complete recovery path.  Crashing before call N also covers the state
  // immediately after call N-1.
  const auto counter_path = NewPath("crash_counter");
  CopyFixture(source, counter_path);
  auto successful_calls = std::size_t{0};
  tinydb::io::SetTestHook([&](const tinydb::io::Call &) -> std::optional<tinydb::io::Fault> {
    ++successful_calls;
    return std::nullopt;
  });
  ASSERT_TRUE(tinydb::recovery::Recover(counter_path, WalPath(counter_path)).Ok());
  tinydb::io::ClearTestHook();
  ASSERT_GT(successful_calls, 0U);
  ExpectRecovered(counter_path);

  for (std::size_t crash_at = 0; crash_at < successful_calls; ++crash_at) {
    const auto db_path = NewPath("crash", crash_at);
    CopyFixture(source, db_path);

    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      auto calls = std::size_t{0};
      tinydb::io::SetTestHook([&](const tinydb::io::Call &) -> std::optional<tinydb::io::Fault> {
        if (calls++ == crash_at) {
          ::kill(::getpid(), SIGKILL);
        }
        return std::nullopt;
      });
      const auto status = tinydb::recovery::Recover(db_path, WalPath(db_path));
      ::_exit(status.Ok() ? 0 : 120);
    }

    auto wait_status = 0;
    ASSERT_EQ(::waitpid(child, &wait_status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(wait_status));
    ASSERT_EQ(WTERMSIG(wait_status), SIGKILL);

    // Recovery is physical and idempotent: partially written pages may be
    // overwritten, and a durable new superblock makes old records skippable.
    ASSERT_TRUE(tinydb::recovery::Recover(db_path, WalPath(db_path)).Ok()) << "crash boundary " << crash_at;
    ExpectRecovered(db_path);
  }
}

TEST_F(RecoveryTest, EveryDurableMiddleRecordClassIsRejectedBeforeRedo) {
  const auto source = NewPath("corrupt_source");
  MakeDatabase(source);
  MakeLinearWal(source, 3);

  const auto offsets_in_transaction = std::array{
      std::uint64_t{0},
      PAGE_IMAGE_RECORD_BYTES,
      PAGE_IMAGE_RECORD_BYTES + DATABASE_STATE_RECORD_BYTES,
  };
  auto instance = std::size_t{0};
  for (std::uint64_t transaction = 0; transaction < 2; ++transaction) {
    for (const auto record_offset : offsets_in_transaction) {
      const auto db_path = NewPath("corrupt", instance++);
      CopyFixture(source, db_path);
      const auto wal_path = WalPath(db_path);
      const auto offset = WAL_HEADER_BYTES + transaction * ONE_PAGE_TRANSACTION_BYTES + record_offset +
                          tinydb::wal_format::RECORD_HEADER_BYTES;
      FlipByte(wal_path, offset);
      const auto database_before = ReadFile(db_path);
      const auto wal_before = ReadFile(wal_path);

      auto database_writes = std::size_t{0};
      tinydb::io::SetTestHook([&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
        if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == db_path) {
          ++database_writes;
        }
        return std::nullopt;
      });
      const auto status = tinydb::recovery::Recover(db_path, wal_path);
      tinydb::io::ClearTestHook();

      EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
      EXPECT_EQ(database_writes, 0U);
      EXPECT_EQ(ReadFile(db_path), database_before);
      EXPECT_EQ(ReadFile(wal_path), wal_before);
    }
  }
}

TEST_F(RecoveryTest, CoveredArchiveLeftByCleanupFailureIsHarmlessAndRemovable) {
  const auto db_path = NewPath("covered_archive");
  MakeDatabase(db_path);
  MakeRotatedWal(db_path);
  const auto archive = tinydb::Wal::SegmentPathFor(WalPath(db_path), 1);

  tinydb::io::SetTestHook([&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Unlink && call.path == archive) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  });
  const auto interrupted = tinydb::recovery::Recover(db_path, WalPath(db_path));
  tinydb::io::ClearTestHook();
  ASSERT_EQ(interrupted.Code(), tinydb::StatusCode::IoError);
  ASSERT_TRUE(std::filesystem::exists(archive));

  // The alternate superblock already covers both transactions.  The retry
  // authenticates the stale archive, skips its covered images, and removes it
  // without advancing the database generation again.
  ASSERT_TRUE(tinydb::recovery::Recover(db_path, WalPath(db_path)).Ok());
  ExpectRecovered(db_path);
}

TEST_F(RecoveryTest, PartiallyAppliedRedoIsRepeatedFromTheOldSuperblock) {
  const auto db_path = NewPath("partial_redo");
  MakeDatabase(db_path);
  const auto first = DataPage(2, 'a');
  const auto second = DataPage(3, 'b');
  {
    auto wal = tinydb::Wal::Open(WalPath(db_path), TEST_UUID);
    ASSERT_TRUE(wal.has_value());
    wal->AppendPageImage(2, first.data());
    wal->AppendPageImage(3, second.data());
    ASSERT_TRUE(wal->Commit(State()).has_value());
  }

  auto page_writes = std::size_t{0};
  tinydb::io::SetTestHook([&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == db_path && ++page_writes == 2) {
      return tinydb::io::Fault{.error = EIO};
    }
    return std::nullopt;
  });
  const auto interrupted = tinydb::recovery::Recover(db_path, WalPath(db_path));
  tinydb::io::ClearTestHook();
  ASSERT_EQ(interrupted.Code(), tinydb::StatusCode::IoError);

  ASSERT_TRUE(tinydb::recovery::Recover(db_path, WalPath(db_path)).Ok());
  const auto database = ReadFile(db_path);
  const auto selected = tinydb::storage::SelectSuperblock(
      std::as_bytes(std::span{database.data(), tinydb::PAGE_SIZE}),
      std::as_bytes(std::span{database.data() + tinydb::PAGE_SIZE, tinydb::PAGE_SIZE}));
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->value.transaction_id, 1U);
  EXPECT_EQ(selected->value.checkpoint_lsn, 4U);
  EXPECT_EQ(database.substr(2 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(first.data(), first.size()));
  EXPECT_EQ(database.substr(3 * tinydb::PAGE_SIZE, tinydb::PAGE_SIZE), std::string(second.data(), second.size()));
  EXPECT_EQ(std::filesystem::file_size(WalPath(db_path)), WAL_HEADER_BYTES);
}

TEST_F(RecoveryTest, ImpossibleFileFrontierIsRejectedBeforePhysicalGrowth) {
  const auto db_path = NewPath("frontier_overflow");
  MakeDatabase(db_path);
  {
    auto wal = tinydb::Wal::Open(WalPath(db_path), TEST_UUID);
    ASSERT_TRUE(wal.has_value());
    const auto page = DataPage(2, 'a');
    wal->AppendPageImage(2, page.data());
    ASSERT_TRUE(wal->Commit(State(std::numeric_limits<tinydb::page_id_t>::max())).has_value());
  }
  const auto database_before = ReadFile(db_path);
  const auto wal_before = ReadFile(WalPath(db_path));

  auto mutating_calls = std::size_t{0};
  tinydb::io::SetTestHook([&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
    if ((call.syscall == tinydb::io::Syscall::Pwrite || call.syscall == tinydb::io::Syscall::Ftruncate) &&
        call.path == db_path) {
      ++mutating_calls;
    }
    return std::nullopt;
  });
  const auto status = tinydb::recovery::Recover(db_path, WalPath(db_path));
  tinydb::io::ClearTestHook();

  EXPECT_EQ(status.Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(mutating_calls, 0U);
  EXPECT_EQ(ReadFile(db_path), database_before);
  EXPECT_EQ(ReadFile(WalPath(db_path)), wal_before);
}

}  // namespace
