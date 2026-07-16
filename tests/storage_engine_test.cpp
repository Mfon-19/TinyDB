#include <gtest/gtest.h>
#include <tinydb/page.h>
#include <tinydb/status.h>
#include <tinydb/storage_engine.h>
#include <tinydb/wal.h>

#include "io/syscalls.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "support/transaction_model.h"
#include "support/transaction_scenarios.h"
#include "txn/database_state.h"
#include "wal/wal_codec.h"

#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
** Storage-engine tests cover behavior across all layers: ordered-map semantics,
** reopen and crash durability, checkpoint ordering, exclusive process
** ownership, churn, corruption reporting, and reader visibility during private
** write preparation. Randomized cases compare every operation with std::map.
*/
namespace {

auto TestUuid(std::byte last = std::byte{1}) -> tinydb::DatabaseUuid {
  auto uuid = tinydb::DatabaseUuid{};
  uuid.back() = last;
  return uuid;
}

// Scan end bound past every key this suite generates.
constexpr const char *SCAN_END = "\x7f";

auto RowKey(int row) -> std::string {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "row-%06d", row);
  return std::string{buffer};
}

auto RowValue(int row, std::size_t length) -> std::string {
  auto value = std::string(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    value[i] = static_cast<char>('a' + (static_cast<std::size_t>(row) + i) % 26);
  }
  return value;
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
  for (auto i = start; i < calls.size(); ++i) {
    if (calls[i].syscall == syscall && calls[i].path == path) {
      return i;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

class StorageEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto stem = "tinydb_engine_" + std::string(info->name()) + "_" + std::to_string(::getpid());
    db_path_ = std::filesystem::temp_directory_path() / (stem + ".db");
    second_db_path_ = std::filesystem::temp_directory_path() / (stem + "_b.db");
    RemoveDatabase(db_path_);
    RemoveDatabase(second_db_path_);
  }

  void TearDown() override {
    RemoveDatabase(db_path_);
    RemoveDatabase(second_db_path_);
  }

  // A database on disk is the file plus its write-ahead log.
  static void RemoveDatabase(const std::filesystem::path &path) {
    std::filesystem::remove(path);
    const auto wal_path = tinydb::Wal::PathFor(path);
    std::filesystem::remove(wal_path);
    auto parent = wal_path.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    const auto prefix = wal_path.filename().string() + ".";
    for (const auto &entry : std::filesystem::directory_iterator(parent)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with(prefix) && name.ends_with(".segment")) {
        std::filesystem::remove(entry.path());
      }
    }
  }

  // Copies the database and its log as they sit on disk right now — the
  // exact state a crash would leave behind (nothing flushed, nothing
  // closed) — so a second engine can be opened on the copy while the
  // original stays live.
  void SnapshotDatabase() const {
    std::filesystem::copy_file(db_path_, second_db_path_);
    const auto source_wal = tinydb::Wal::PathFor(db_path_);
    const auto destination_wal = tinydb::Wal::PathFor(second_db_path_);
    std::filesystem::copy_file(source_wal, destination_wal);
    auto parent = source_wal.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    const auto prefix = source_wal.filename().string() + ".";
    for (const auto &entry : std::filesystem::directory_iterator(parent)) {
      const auto name = entry.path().filename().string();
      if (!name.starts_with(prefix) || !name.ends_with(".segment")) {
        continue;
      }
      const auto suffix = name.substr(source_wal.filename().string().size());
      auto destination = destination_wal;
      destination += suffix;
      std::filesystem::copy_file(entry.path(), destination);
    }
  }

  std::filesystem::path db_path_;
  std::filesystem::path second_db_path_;
};

/*
** The reusable contract scenarios intentionally speak a tiny model-shaped
** vocabulary. This adapter removes Result/Status wrappers only after asserting
** that TinyDB returned a value, leaving the scenarios themselves independent
** of storage implementation types.
*/
class TinyDbContractAdapter final {
 public:
  using Row = std::pair<std::string, std::string>;
  using Rows = std::vector<Row>;

  explicit TinyDbContractAdapter(tinydb::StorageEngine *engine) : engine_(engine) {}

  class Write final {
   public:
    explicit Write(tinydb::WriteTransaction transaction) : transaction_(std::move(transaction)) {}

    auto Put(std::string_view key, std::string_view value) -> tinydb::StatusCode {
      return transaction_.Put(key, value).Code();
    }
    auto Delete(std::string_view key) -> tinydb::StatusCode { return transaction_.Delete(key).Code(); }
    auto Get(std::string_view key) -> std::optional<std::string> { return transaction_.Get(key).value(); }
    auto Commit() -> bool { return transaction_.Commit().has_value(); }
    void Abort() noexcept { transaction_.Abort(); }

   private:
    tinydb::WriteTransaction transaction_;
  };

  auto BeginWrite() -> std::optional<Write> {
    auto transaction = engine_->BeginWrite();
    if (!transaction) {
      return std::nullopt;
    }
    return Write(std::move(*transaction));
  }

  auto Get(std::string_view key) -> std::optional<std::string> { return engine_->Get(key).value(); }

  auto Scan(std::optional<std::string_view> start = std::nullopt,
            std::optional<std::string_view> end = std::nullopt) -> Rows {
    auto range = tinydb::KeyRange::All();
    if (start && end) {
      range = tinydb::KeyRange::Between(*start, *end);
    } else if (start) {
      range = tinydb::KeyRange::From(*start);
    } else if (end) {
      range = tinydb::KeyRange::Until(*end);
    }
    auto transaction = engine_->BeginRead().value();
    auto cursor = transaction.Scan(std::move(range)).value();
    auto rows = Rows{};
    while (cursor.Valid()) {
      rows.emplace_back(cursor.Key(), cursor.CopyValue().value());
      EXPECT_TRUE(cursor.Next().Ok());
    }
    return rows;
  }

 private:
  tinydb::StorageEngine *engine_;
};

TEST_F(StorageEngineTest, PublicTransactionCommitPublishesAtomically) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::CommitPublishesAtomically(adapter);
}

TEST_F(StorageEngineTest, PublicTransactionAbortDiscardsAllChanges) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::AbortDiscardsAllChanges(adapter);
}

TEST_F(StorageEngineTest, PublicTransactionDestructionAbortsAndReleasesWriter) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::DestructionAborts(adapter);
}

TEST_F(StorageEngineTest, PublicTransactionReadsOwnWritesAndDeletes) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::OverwriteDeleteAndReadOwnWrites(adapter);
}

TEST_F(StorageEngineTest, PublicTransactionScansUseHalfOpenOptionalBounds) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::ScanUsesHalfOpenOptionalBounds(adapter);
}

TEST_F(StorageEngineTest, PublicTransactionKeysUseUnsignedByteOrder) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::KeysUseUnsignedByteOrder(adapter);
}

TEST_F(StorageEngineTest, PublicTransactionAllowsOnlyOneWriter) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto adapter = TinyDbContractAdapter(&engine);
  tinydb::test_support::OnlyOneWriterIsAdmitted(adapter);
}

TEST_F(StorageEngineTest, InvalidMutationLeavesTheWriteTransactionActive) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto transaction = engine.BeginWrite().value();
  const auto oversized_key = std::string(tinydb::txn::MAX_KEY_BYTES + 1, 'x');
  EXPECT_EQ(transaction.Put(oversized_key, "value").Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(transaction.Delete(oversized_key).Code(), tinydb::StatusCode::InvalidArgument);
  ASSERT_TRUE(transaction.Put("valid", "value").Ok());
  ASSERT_TRUE(transaction.Commit().has_value());
  EXPECT_EQ(engine.Get("valid").value(), std::optional<std::string>{"value"});
}

TEST_F(StorageEngineTest, PublicTransactionCloseIsBusyWithoutInvalidatingTransactions) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  {
    auto read = engine.BeginRead().value();
    EXPECT_EQ(engine.Close().Code(), tinydb::StatusCode::Busy);
    EXPECT_EQ(read.Get("missing").value(), std::nullopt);
  }
  {
    auto write = engine.BeginWrite().value();
    EXPECT_EQ(engine.Close().Code(), tinydb::StatusCode::Busy);
    ASSERT_TRUE(write.Put("key", "value").Ok());
    ASSERT_TRUE(write.Commit().has_value());
  }
  EXPECT_TRUE(engine.Close().Ok());
}

TEST_F(StorageEngineTest, PublicReadTransactionKeepsOneSnapshotUntilWriterPublishes) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("key", "before").Ok());
  auto reader = std::optional<tinydb::ReadTransaction>{engine.BeginRead().value()};

  auto started = std::atomic<bool>{false};
  auto finished = std::atomic<bool>{false};
  auto writer = std::thread([&] {
    auto transaction = engine.BeginWrite().value();
    ASSERT_TRUE(transaction.Put("key", "after").Ok());
    started.store(true, std::memory_order_release);
    const auto committed = transaction.Commit();
    EXPECT_TRUE(committed.has_value());
    finished.store(true, std::memory_order_release);
  });
  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_EQ(reader->Get("key").value(), std::optional<std::string>{"before"});
  EXPECT_FALSE(finished.load(std::memory_order_acquire));
  reader.reset();
  writer.join();
  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{"after"});
}

TEST_F(StorageEngineTest, MultiPageTransactionSurvivesCrashAsOneCommittedUnit) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto transaction = engine.BeginWrite().value();
  for (int row = 0; row < 80; ++row) {
    ASSERT_TRUE(transaction.Put(RowKey(row), RowValue(row, 256)).Ok());
  }
  const auto committed = transaction.Commit();
  ASSERT_TRUE(committed.has_value());
  EXPECT_GT(committed->transaction_id, 0U);
  EXPECT_GT(committed->commit_lsn, 0U);

  SnapshotDatabase();
  auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();
  for (int row = 0; row < 80; ++row) {
    EXPECT_EQ(recovered.Get(RowKey(row)).value(), std::optional<std::string>{RowValue(row, 256)});
  }
}

TEST_F(StorageEngineTest, CrashAfterWalDurabilityBeforePublicationRecoversTransaction) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  auto reader = std::optional<tinydb::ReadTransaction>{engine.BeginRead().value()};
  auto wal_synced = std::atomic<bool>{false};
  auto writer_done = std::atomic<bool>{false};

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Fsync && call.path == tinydb::Wal::PathFor(db_path_)) {
        wal_synced.store(true, std::memory_order_release);
      }
      return std::nullopt;
    }};
    auto writer = std::thread([&] {
      auto transaction = engine.BeginWrite().value();
      ASSERT_TRUE(transaction.Put("doc/1", "contents").Ok());
      ASSERT_TRUE(transaction.Put("tag/database/doc/1", "").Ok());
      EXPECT_TRUE(transaction.Commit().has_value());
      writer_done.store(true, std::memory_order_release);
    });

    while (!wal_synced.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_FALSE(writer_done.load(std::memory_order_acquire));
    EXPECT_EQ(reader->Get("doc/1").value(), std::nullopt);

    // This copy is the exact durable-but-not-yet-visible crash state. Recovery
    // must publish both keys from physical WAL without relying on the blocked
    // writer's in-memory publication plan.
    SnapshotDatabase();
    auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();
    EXPECT_EQ(recovered.Get("doc/1").value(), std::optional<std::string>{"contents"});
    EXPECT_EQ(recovered.Get("tag/database/doc/1").value(), std::optional<std::string>{""});

    reader.reset();
    writer.join();
  }
  EXPECT_TRUE(writer_done.load(std::memory_order_acquire));
}

TEST_F(StorageEngineTest, RepairedWalAppendFailureIsADefiniteAbort) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == wal_path) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    EXPECT_EQ(engine.Put("aborted", "value").Code(), tinydb::StatusCode::IoError);
  }

  EXPECT_EQ(engine.Get("aborted").value(), std::nullopt);
  EXPECT_TRUE(engine.Put("committed", "value").Ok());
  EXPECT_EQ(engine.Get("committed").value(), std::optional<std::string>{"value"});
}

TEST_F(StorageEngineTest, WalSyncFailureMakesCommitIndeterminateAndRequiresReopen) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Fsync && call.path == wal_path) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    EXPECT_EQ(engine.Put("limbo", "value").Code(), tinydb::StatusCode::IndeterminateCommit);
  }

  EXPECT_EQ(engine.Get("limbo").error().Code(), tinydb::StatusCode::NeedsRecovery);
  EXPECT_EQ(engine.Put("later", "value").Code(), tinydb::StatusCode::NeedsRecovery);
  EXPECT_TRUE(engine.Close().Ok());

  auto recovered = tinydb::StorageEngine::Open(db_path_).value();
  const auto value = recovered.Get("limbo").value();
  EXPECT_TRUE(value == std::nullopt || value == std::optional<std::string>{"value"});
}

TEST_F(StorageEngineTest, OpenCreatesEmptyDatabase) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  EXPECT_TRUE(std::filesystem::exists(db_path_));
  EXPECT_EQ(engine.Get("anything").value(), std::nullopt);
  EXPECT_TRUE(engine.Scan("", SCAN_END).value().empty());
  EXPECT_TRUE(engine.Remove("anything").Ok());  // removing from an empty database is a no-op
}

TEST_F(StorageEngineTest, PutGetRemoveRoundTrip) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  EXPECT_TRUE(engine.Put("apple", "red").Ok());
  EXPECT_TRUE(engine.Put("banana", "yellow").Ok());

  EXPECT_EQ(engine.Get("apple").value(), std::optional<std::string>{"red"});
  EXPECT_EQ(engine.Get("banana").value(), std::optional<std::string>{"yellow"});

  EXPECT_TRUE(engine.Remove("apple").Ok());
  EXPECT_EQ(engine.Get("apple").value(), std::nullopt);
  EXPECT_EQ(engine.Get("banana").value(), std::optional<std::string>{"yellow"});
}

TEST_F(StorageEngineTest, IntegrityCheckSurvivesSplitsMergesAndReopen) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    for (int row = 0; row < 500; ++row) {
      ASSERT_TRUE(engine.Put(RowKey(row), RowValue(row, 700)).Ok());
    }
    for (int row = 0; row < 350; ++row) {
      ASSERT_TRUE(engine.Remove(RowKey(row)).Ok());
    }
    EXPECT_TRUE(engine.CheckIntegrity().Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  auto reopened = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_TRUE(reopened.CheckIntegrity().Ok());
  EXPECT_EQ(reopened.Scan("", SCAN_END).value().size(), 150U);
}

TEST_F(StorageEngineTest, RandomizedModelSurvivesCheckpointsAndReopens) {
  auto expected = tinydb::test_support::TransactionModel{};
  auto random = std::mt19937_64{0x54494e594442ULL};

  for (int epoch = 0; epoch < 10; ++epoch) {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    for (int step = 0; step < 200; ++step) {
      const auto row = static_cast<int>(random() % 300U);
      const auto key = RowKey(row);
      const auto action = random() % 4U;
      if (action < 2U) {
        const auto length = static_cast<std::size_t>((random() % 850U) + 1U);
        const auto value = RowValue(row + epoch + step, length);
        ASSERT_TRUE(engine.Put(key, value).Ok());
        auto transaction = expected.BeginWrite();
        ASSERT_TRUE(transaction.has_value());
        ASSERT_EQ(transaction->Put(key, value), tinydb::StatusCode::Ok);
        ASSERT_TRUE(transaction->Commit());
      } else if (action == 2U) {
        ASSERT_TRUE(engine.Remove(key).Ok());
        auto transaction = expected.BeginWrite();
        ASSERT_TRUE(transaction.has_value());
        ASSERT_EQ(transaction->Delete(key), tinydb::StatusCode::Ok);
        ASSERT_TRUE(transaction->Commit());
      } else {
        const auto found = engine.Get(key);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, expected.Get(key));
      }
    }

    const auto integrity = engine.CheckIntegrity();
    ASSERT_TRUE(integrity.Ok()) << integrity.ToString();
    const auto rows = engine.Scan("", SCAN_END).value();
    EXPECT_EQ(rows, expected.Scan());
    ASSERT_TRUE(engine.Close().Ok());
  }
}

TEST_F(StorageEngineTest, PutReplacesExistingValue) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  EXPECT_TRUE(engine.Put("key", "first").Ok());
  EXPECT_TRUE(engine.Put("key", "second").Ok());

  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{"second"});
  EXPECT_EQ(engine.Scan("", SCAN_END).value().size(), 1);
}

TEST_F(StorageEngineTest, EntrySizeCapIsEnforced) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  // Exactly at the cap is accepted.
  const auto key = std::string{"key"};
  const auto max_value = RowValue(0, tinydb::MAX_ENTRY_BYTES - key.size());
  EXPECT_TRUE(engine.Put(key, max_value).Ok());

  // One byte over is rejected and must not disturb existing data.
  const auto oversized = max_value + "x";
  EXPECT_EQ(engine.Put("other", oversized).Code(), tinydb::StatusCode::InvalidArgument);
  EXPECT_EQ(engine.Put(key, oversized).Code(), tinydb::StatusCode::InvalidArgument);

  EXPECT_EQ(engine.Get("other").value(), std::nullopt);
  EXPECT_EQ(engine.Get(key).value(), std::optional<std::string>{max_value});
}

TEST_F(StorageEngineTest, ScanBoundsAreHalfOpen) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(engine.Put(RowKey(i), RowValue(i, 20)).Ok());
  }

  const auto rows = engine.Scan(RowKey(3), RowKey(6)).value();
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows.front().first, RowKey(3));
  EXPECT_EQ(rows.back().first, RowKey(5));

  EXPECT_TRUE(engine.Scan(RowKey(4), RowKey(4)).value().empty());
}

TEST_F(StorageEngineTest, ClosedHandleRefusesWork) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("key", "value").Ok());

  ASSERT_TRUE(engine.Close().Ok());

  EXPECT_EQ(engine.Put("key", "other").Code(), tinydb::StatusCode::Closed);
  EXPECT_EQ(engine.Remove("key").Code(), tinydb::StatusCode::Closed);
  const auto got = engine.Get("key");
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().Code(), tinydb::StatusCode::Closed);
  const auto rows = engine.Scan("", SCAN_END);
  ASSERT_FALSE(rows.has_value());
  EXPECT_EQ(rows.error().Code(), tinydb::StatusCode::Closed);
  EXPECT_TRUE(engine.Close().Ok());  // closing twice is safe

  // The pre-close write reached the file.
  auto reopened = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(reopened.Get("key").value(), std::optional<std::string>{"value"});
}

TEST_F(StorageEngineTest, OpenFailsWhileAnotherHandleHoldsTheDatabase) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put(RowKey(1), "first").Ok());

  // The exclusive lock turns a second Open away — before its recovery
  // could truncate the log the live handle is still appending to.
  const auto second = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().Code(), tinydb::StatusCode::Busy);

  // The live handle is unharmed by the refused attempt.
  ASSERT_TRUE(engine.Put(RowKey(2), "second").Ok());

  // Close releases the lock, and everything written under it survives.
  ASSERT_TRUE(engine.Close().Ok());
  auto reopened = tinydb::StorageEngine::Open(db_path_);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->Get(RowKey(1)).value(), std::optional<std::string>{"first"});
  EXPECT_EQ(reopened->Get(RowKey(2)).value(), std::optional<std::string>{"second"});
}

TEST_F(StorageEngineTest, DataSurvivesExplicitClose) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("persist", "me").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get("persist").value(), std::optional<std::string>{"me"});
}

TEST_F(StorageEngineTest, DataSurvivesDestructor) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("persist", "me too").Ok());
    // No Close(): the destructor must flush.
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get("persist").value(), std::optional<std::string>{"me too"});
}

TEST_F(StorageEngineTest, ReopenedDatabaseAcceptsMutations) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    for (int i = 0; i < 20; ++i) {
      ASSERT_TRUE(engine.Put(RowKey(i), RowValue(i, 30)).Ok());
    }
  }
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Remove(RowKey(5)).Ok());
    ASSERT_TRUE(engine.Put(RowKey(100), RowValue(100, 30)).Ok());
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get(RowKey(5)).value(), std::nullopt);
  EXPECT_EQ(engine.Get(RowKey(100)).value(), std::optional<std::string>{RowValue(100, 30)});
  EXPECT_EQ(engine.Scan("", SCAN_END).value().size(), 20);  // 20 - 1 + 1
}

TEST_F(StorageEngineTest, LargeWorkloadSurvivesReopen) {
  auto model = std::map<std::string, std::string>{};

  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    // Enough data for many leaf splits and at least one root split.
    for (int i = 0; i < 400; ++i) {
      const auto key = RowKey(i);
      const auto value = RowValue(i, 50 + (static_cast<std::size_t>(i) * 13) % 400);
      ASSERT_TRUE(engine.Put(key, value).Ok());
      model[key] = value;
    }
    for (int i = 0; i < 400; i += 3) {
      ASSERT_TRUE(engine.Remove(RowKey(i)).Ok());
      model.erase(RowKey(i));
    }
  }

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  const auto rows = engine.Scan("", SCAN_END).value();
  ASSERT_EQ(rows.size(), model.size());
  auto it = model.begin();
  for (const auto &[key, value] : rows) {
    EXPECT_EQ(key, it->first);
    EXPECT_EQ(value, it->second);
    ++it;
  }
}

TEST_F(StorageEngineTest, ChurnDoesNotGrowTheFile) {
  const auto churn = [this] {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(engine.Put(RowKey(i), RowValue(i, 120)).Ok());
    }
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(engine.Remove(RowKey(i)).Ok());
    }
    ASSERT_TRUE(engine.Close().Ok());
  };

  // The first cycle sizes the file; merges and root collapses free the
  // emptied pages, so every later identical cycle runs on reused pages.
  churn();
  const auto stable_size = std::filesystem::file_size(db_path_);
  for (int cycle = 0; cycle < 3; ++cycle) {
    churn();
    EXPECT_EQ(std::filesystem::file_size(db_path_), stable_size);
  }
}

TEST_F(StorageEngineTest, MoveTransfersOwnership) {
  auto first = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(first.Put("moved", "data").Ok());

  auto second = std::move(first);

  // The moved-from handle acts closed.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(first.Put("x", "y").Code(), tinydb::StatusCode::Closed);

  // The destination owns the database.
  EXPECT_EQ(second.Get("moved").value(), std::optional<std::string>{"data"});
  EXPECT_TRUE(second.Put("more", "rows").Ok());
}

TEST_F(StorageEngineTest, MoveAssignClosesTheOldDatabase) {
  auto target = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(target.Put("old", "database").Ok());

  auto source = tinydb::StorageEngine::Open(second_db_path_).value();
  ASSERT_TRUE(source.Put("new", "database").Ok());

  target = std::move(source);

  // target now serves the second database...
  EXPECT_EQ(target.Get("new").value(), std::optional<std::string>{"database"});
  EXPECT_EQ(target.Get("old").value(), std::nullopt);

  // ...and the first database was flushed when target closed it.
  auto reopened = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(reopened.Get("old").value(), std::optional<std::string>{"database"});
}

TEST_F(StorageEngineTest, OpenRejectsForeignFiles) {
  {
    auto file = std::ofstream{db_path_};
    file << "this is not a tinydb database, just some text\n";
  }

  const auto result = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::UnsupportedFormat);
}

TEST_F(StorageEngineTest, OpenRejectsForeignFilesBeforeWalReplay) {
  {
    auto file = std::ofstream{db_path_, std::ios::binary | std::ios::trunc};
    file << "this is not a tinydb database, just some text\n";
  }
  const auto db_before = [&] {
    auto file = std::ifstream{db_path_, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }();

  {
    const auto uuid = TestUuid();
    auto wal = tinydb::Wal::Open(tinydb::Wal::PathFor(db_path_), uuid).value();
    const auto payload = std::array{std::byte{'x'}};
    const auto image = tinydb::storage::EncodeOverflowPage(2, 1, 1, tinydb::HEADER_PAGE_ID, payload);
    ASSERT_TRUE(image.has_value());
    wal.AppendPageImage(2, image->data());
    const auto committed = wal.Commit(tinydb::txn::DatabaseState{
        .root_page_id = tinydb::HEADER_PAGE_ID,
        .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
        .high_water_page_id = 3,
    });
    ASSERT_TRUE(committed.has_value());
  }
  const auto wal_size_before = std::filesystem::file_size(tinydb::Wal::PathFor(db_path_));

  const auto result = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::UnsupportedFormat);

  {
    auto file = std::ifstream{db_path_, std::ios::binary};
    const auto db_after = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(db_after, db_before);
  }
  EXPECT_EQ(std::filesystem::file_size(tinydb::Wal::PathFor(db_path_)), wal_size_before);
}

TEST_F(StorageEngineTest, OpenReturnsCorruptionForADamagedDataPage) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("key", "value").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  {
    auto file = std::fstream{db_path_, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(file.is_open());
    file.seekp(static_cast<std::streamoff>(tinydb::FIRST_DATA_PAGE_ID) * tinydb::PAGE_SIZE);
    const char damaged_magic = '\0';
    file.write(&damaged_magic, 1);
    ASSERT_TRUE(file.good());
  }

  const auto result = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::Corruption);
}

}  // namespace

TEST_F(StorageEngineTest, OpenRefusesAWalFromAnotherDatabase) {
  // Two healthy databases, each with its own UUID and its own (empty) log.
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put(RowKey(1), "mine").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }
  {
    auto engine = tinydb::StorageEngine::Open(second_db_path_).value();
    ASSERT_TRUE(engine.Put(RowKey(1), "theirs").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  // Pair the first database with the second one's log — the copy-paste
  // accident the UUID exists to catch.
  std::filesystem::remove(tinydb::Wal::PathFor(db_path_));
  std::filesystem::copy_file(tinydb::Wal::PathFor(second_db_path_), tinydb::Wal::PathFor(db_path_));

  const auto result = tinydb::StorageEngine::Open(db_path_);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().Code(), tinydb::StatusCode::InvalidArgument);
}

TEST_F(StorageEngineTest, CommittedWritesSurviveACrash) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  for (int row = 0; row < 200; ++row) {
    ASSERT_TRUE(engine.Put(RowKey(row), RowValue(row, 64)).Ok());
  }
  for (int row = 0; row < 100; ++row) {
    ASSERT_TRUE(engine.Remove(RowKey(row)).Ok());
  }

  // "Crash": snapshot the files mid-flight, with the engine still open and
  // nothing checkpointed, and recover a second engine from the copy.
  SnapshotDatabase();
  auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();

  const auto rows = recovered.Scan("", SCAN_END).value();
  ASSERT_EQ(rows.size(), 100U);
  for (int row = 100; row < 200; ++row) {
    EXPECT_EQ(recovered.Get(RowKey(row)).value(), RowValue(row, 64));
  }
  EXPECT_EQ(recovered.Get(RowKey(0)).value(), std::nullopt);

  // Recovery leaves the copy a normal database: it accepts new work.
  ASSERT_TRUE(recovered.Put(RowKey(0), "back again").Ok());
  EXPECT_TRUE(recovered.Close().Ok());
}

TEST_F(StorageEngineTest, CleanCloseLeavesCoveredActiveWalForRecoveryCleanup) {
  {
    auto engine = tinydb::StorageEngine::Open(db_path_).value();
    ASSERT_TRUE(engine.Put("k", "v").Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }

  // Ordinary checkpointing never truncates its active append target. The
  // durable superblock covers these retained records, so reopen can validate
  // and discard them without using them as redo.
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  EXPECT_GT(std::filesystem::file_size(wal_path), tinydb::wal_format::HEADER_BYTES);

  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  EXPECT_EQ(engine.Get("k").value(), "v");
  EXPECT_EQ(std::filesystem::file_size(wal_path), tinydb::wal_format::HEADER_BYTES);
}

TEST_F(StorageEngineTest, CleanCloseSyncsPagesBeforePublishingTheInactiveSuperblock) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("k", "v").Ok());

  auto calls = std::vector<tinydb::io::Call>{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      calls.push_back(call);
      return std::nullopt;
    }};
    ASSERT_TRUE(engine.Close().Ok());
  }

  const auto page_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path_);
  const auto superblock_write = FindCall(calls, tinydb::io::Syscall::Pwrite, db_path_, page_sync + 1);
  const auto superblock_sync = FindCall(calls, tinydb::io::Syscall::Fsync, db_path_, superblock_write + 1);
  const auto wal_truncate = FindCall(calls, tinydb::io::Syscall::Ftruncate, tinydb::Wal::PathFor(db_path_));

  ASSERT_NE(page_sync, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(superblock_write, std::numeric_limits<std::size_t>::max());
  ASSERT_NE(superblock_sync, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(page_sync, superblock_write);
  EXPECT_LT(superblock_write, superblock_sync);
  EXPECT_EQ(wal_truncate, std::numeric_limits<std::size_t>::max());
}

TEST_F(StorageEngineTest, CleanCloseLeavesWalIntactWhenDatabaseSyncFails) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("k", "v").Ok());
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  const auto wal_size_before = std::filesystem::file_size(wal_path);

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Fsync && call.path == db_path_) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto status = engine.Close();
    EXPECT_EQ(status.Code(), tinydb::StatusCode::IoError);
    EXPECT_EQ(std::filesystem::file_size(wal_path), wal_size_before);
  }

  ASSERT_TRUE(engine.Close().Ok());
}

TEST_F(StorageEngineTest, WritePublishingDuringCheckpointRecoversFromTheNewCheckpointBase) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("key", "before").Ok());

  auto mutex = std::mutex{};
  auto changed = std::condition_variable{};
  auto page_write_reached = false;
  auto release_page_write = false;
  auto checkpoint_status = tinydb::Status{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == db_path_ &&
          call.offset >= tinydb::FIRST_DATA_PAGE_ID * tinydb::PAGE_SIZE) {
        auto lock = std::unique_lock(mutex);
        page_write_reached = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release_page_write; });
      }
      return std::nullopt;
    }};
    auto checkpoint = std::thread([&] { checkpoint_status = engine.Checkpoint(); });
    {
      auto lock = std::unique_lock(mutex);
      changed.wait(lock, [&] { return page_write_reached; });
    }

    // This transaction began from the older checkpoint frontier. Its WAL
    // record retains that conservative allocator base, while publication later
    // merges the checkpoint that is currently writing.
    ASSERT_TRUE(engine.Put("key", "after").Ok());
    {
      auto lock = std::lock_guard(mutex);
      release_page_write = true;
    }
    changed.notify_all();
    checkpoint.join();
  }
  ASSERT_TRUE(checkpoint_status.Ok()) << checkpoint_status.ToString();
  EXPECT_EQ(engine.Get("key").value(), "after");

  SnapshotDatabase();
  auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();
  EXPECT_EQ(recovered.Get("key").value(), "after");
}

TEST_F(StorageEngineTest, LogOutgrowingItsThresholdCheckpoints) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();

  // Each put logs full images of every page it touches, so this comfortably
  // pushes the log past its 1 MiB checkpoint threshold at least once.
  for (int row = 0; row < 400; ++row) {
    ASSERT_TRUE(engine.Put(RowKey(row), RowValue(row, 128)).Ok());
  }

  // The workload appended well over 1 MiB of images in total, so a log
  // still at or under the threshold proves at least one reset happened.
  const auto wal_path = tinydb::Wal::PathFor(db_path_);
  EXPECT_LE(std::filesystem::file_size(wal_path), (1U << 20U));

  // And a post-checkpoint crash still recovers cleanly: the database file
  // plus the shorter log reproduce every row.
  SnapshotDatabase();
  auto recovered = tinydb::StorageEngine::Open(second_db_path_).value();
  EXPECT_EQ(recovered.Scan("", SCAN_END).value().size(), 400U);
  EXPECT_EQ(recovered.Get(RowKey(399)).value(), RowValue(399, 128));
}

TEST_F(StorageEngineTest, ReadersSeeOnlyWholeCommittedValuesDuringWrites) {
  auto engine = tinydb::StorageEngine::Open(db_path_).value();
  ASSERT_TRUE(engine.Put("shared", "value-0").Ok());
  auto stop = std::atomic<bool>{false};
  auto failures = std::atomic<std::size_t>{0};
  auto readers = std::vector<std::thread>{};
  for (int index = 0; index < 6; ++index) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const auto value = engine.Get("shared");
        if (!value || !value->has_value() || !value->value().starts_with("value-")) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (int version = 1; version <= 100; ++version) {
    if (!engine.Put("shared", "value-" + std::to_string(version)).Ok()) {
      failures.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
  stop.store(true, std::memory_order_release);
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(engine.Get("shared").value(), std::optional<std::string>{"value-100"});
}
