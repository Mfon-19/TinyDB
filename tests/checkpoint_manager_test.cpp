#include <gtest/gtest.h>

#include "storage/disk_manager.h"
#include "wal/wal.h"

#include "cache/committed_page_cache.h"
#include "checkpoint/checkpoint_manager.h"
#include "io/syscalls.h"
#include "storage/page_codec.h"
#include "storage/superblock.h"
#include "txn/database_state.h"
#include "txn/reader_gate.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/*
** IMMUTABLE CHECKPOINT TEST MODEL
**
** Each fixture begins with two durable empty superblocks and publishes valid
** checksummed overflow pages directly into the committed cache. The WAL is an
** empty writer because these tests isolate checkpoint ownership and ordering;
** transaction/WAL binding is covered by the commit and recovery suites.
**
** The concurrency test pauses the checkpoint at its first data-page write.
** At that instant capture has released the publication gate but still pins the
** old frame. Publishing a newer frame must neither change the retained bytes
** nor let MarkCheckpointed make the newer frame evictable.
*/

using tinydb::cache::CommittedPageCache;
using tinydb::cache::CommittedPageImage;
using tinydb::cache::PageBytes;

auto TestPath(std::string_view name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tinydb_checkpoint_" + std::string(name) + "_" + std::to_string(::getpid()) + ".db");
}

auto EncodedPage(tinydb::page_id_t page_id, std::uint64_t lsn, std::byte marker) -> std::unique_ptr<PageBytes> {
  const auto payload = std::array{marker};
  auto encoded = tinydb::storage::EncodeOverflowPage(page_id, lsn, page_id, 0, tinydb::HEADER_PAGE_ID, payload);
  EXPECT_TRUE(encoded.has_value());
  return std::make_unique<PageBytes>(std::move(*encoded));
}

auto Marker(std::span<const char, tinydb::PAGE_SIZE> bytes, tinydb::page_id_t page_id) -> std::byte {
  const auto decoded = tinydb::storage::DecodeOverflowPage(std::as_bytes(bytes), page_id);
  EXPECT_TRUE(decoded.has_value());
  return decoded->payload.front();
}

class ScopedSyscallHook final {
 public:
  explicit ScopedSyscallHook(tinydb::io::TestHook hook) { tinydb::io::SetTestHook(std::move(hook)); }
  ScopedSyscallHook(const ScopedSyscallHook &) = delete;
  auto operator=(const ScopedSyscallHook &) -> ScopedSyscallHook & = delete;
  ~ScopedSyscallHook() { tinydb::io::ClearTestHook(); }
};

struct Fixture final {
  explicit Fixture(std::string_view name) : path(TestPath(name)), wal_path(tinydb::Wal::PathFor(path)) {
    std::filesystem::remove(path);
    std::filesystem::remove(wal_path);
    disk = std::make_unique<tinydb::DiskManager>(tinydb::DiskManager::Open(path).value());
    cache = std::make_unique<CommittedPageCache>(disk.get(), 8 * tinydb::PAGE_SIZE, 0);
    readers = std::make_unique<tinydb::txn::ReaderGate>(std::make_shared<const tinydb::txn::DatabaseState>());
    wal = std::make_unique<tinydb::Wal>(tinydb::Wal::Open(wal_path, disk->Uuid()).value());
  }

  ~Fixture() {
    tinydb::io::ClearTestHook();
    manager.reset();
    wal.reset();
    readers.reset();
    cache.reset();
    disk.reset();
    std::filesystem::remove(wal_path);
    std::filesystem::remove(path);
  }

  void UsePolicy(tinydb::checkpoint::Policy policy = {}) {
    manager = std::make_unique<tinydb::checkpoint::Manager>(disk.get(), cache.get(), readers.get(), wal.get(), policy);
  }

  void Publish(std::uint64_t lsn, std::uint64_t transaction_id, std::byte marker) {
    const auto page_id = tinydb::FIRST_DATA_PAGE_ID;
    auto images = std::vector<CommittedPageImage>{};
    images.push_back(CommittedPageImage{
        .page_id = page_id,
        .page_lsn = lsn,
        .transaction_id = transaction_id,
        .bytes = EncodedPage(page_id, lsn, marker),
    });
    auto plan = cache->PreparePublication(std::move(images), {}, page_id + 1);
    ASSERT_TRUE(plan.has_value());

    auto publication = readers->BeginPublication();
    const auto checkpoint_lsn = publication.CurrentState()->checkpoint_lsn;
    cache->Publish(std::move(*plan));
    publication.Publish(std::make_shared<const tinydb::txn::DatabaseState>(tinydb::txn::DatabaseState{
        .root_page_id = page_id,
        .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
        .high_water_page_id = page_id + 1,
        .transaction_id = transaction_id,
        .visible_lsn = lsn,
        .checkpoint_lsn = checkpoint_lsn,
    }));
  }

  std::filesystem::path path;
  std::filesystem::path wal_path;
  std::unique_ptr<tinydb::DiskManager> disk;
  std::unique_ptr<CommittedPageCache> cache;
  std::unique_ptr<tinydb::txn::ReaderGate> readers;
  std::unique_ptr<tinydb::Wal> wal;
  std::unique_ptr<tinydb::checkpoint::Manager> manager;
};

}  // namespace

TEST(CheckpointManagerTest, RetainedOldFrameCannotOverwriteNewerVisibleVersion) {
  auto fixture = Fixture("retained_version");
  fixture.UsePolicy();
  fixture.Publish(5, 1, std::byte{0x15});

  auto mutex = std::mutex{};
  auto changed = std::condition_variable{};
  auto page_write_reached = false;
  auto release_page_write = false;
  auto checkpoint_status = tinydb::Status{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == fixture.path &&
          call.offset == tinydb::FIRST_DATA_PAGE_ID * tinydb::PAGE_SIZE) {
        auto lock = std::unique_lock(mutex);
        page_write_reached = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release_page_write; });
      }
      return std::nullopt;
    }};
    auto checkpoint = std::thread([&] { checkpoint_status = fixture.manager->Checkpoint(); });
    {
      auto lock = std::unique_lock(mutex);
      changed.wait(lock, [&] { return page_write_reached; });
    }

    // Capture retained P@5. This publication replaces only the cache's current
    // pointer; the checkpoint thread still owns the old guard.
    fixture.Publish(8, 2, std::byte{0x28});
    {
      auto lock = std::lock_guard(mutex);
      release_page_write = true;
    }
    changed.notify_all();
    checkpoint.join();
  }

  ASSERT_TRUE(checkpoint_status.Ok()) << checkpoint_status.ToString();
  EXPECT_EQ(fixture.disk->CheckpointLsn(), 5U);
  EXPECT_EQ(fixture.readers->CurrentState()->visible_lsn, 8U);
  EXPECT_EQ(fixture.readers->CurrentState()->checkpoint_lsn, 5U);
  EXPECT_EQ(fixture.cache->DirtyPageIds(), std::vector<tinydb::page_id_t>{tinydb::FIRST_DATA_PAGE_ID});

  auto visible = fixture.cache->Read(tinydb::FIRST_DATA_PAGE_ID).value();
  EXPECT_EQ(Marker(visible.Data(), visible.Id()), std::byte{0x28});
  auto disk_page = std::array<char, tinydb::PAGE_SIZE>{};
  ASSERT_TRUE(fixture.disk->ReadPage(tinydb::FIRST_DATA_PAGE_ID, disk_page.data()).Ok());
  EXPECT_EQ(Marker(disk_page, tinydb::FIRST_DATA_PAGE_ID), std::byte{0x15});

  ASSERT_TRUE(fixture.manager->Checkpoint().Ok());
  EXPECT_EQ(fixture.disk->CheckpointLsn(), 8U);
  EXPECT_TRUE(fixture.cache->DirtyPageIds().empty());
  ASSERT_TRUE(fixture.disk->ReadPage(tinydb::FIRST_DATA_PAGE_ID, disk_page.data()).Ok());
  EXPECT_EQ(Marker(disk_page, tinydb::FIRST_DATA_PAGE_ID), std::byte{0x28});
}

TEST(CheckpointManagerTest, ActiveReaderDoesNotDelaySnapshotCaptureOrPageWrites) {
  auto fixture = Fixture("reader_during_checkpoint");
  fixture.UsePolicy();
  fixture.Publish(6, 1, std::byte{0x26});
  auto reader = fixture.readers->BeginRead();

  auto mutex = std::mutex{};
  auto changed = std::condition_variable{};
  auto page_write_reached = false;
  auto checkpoint_done = false;
  auto checkpoint_status = tinydb::Status{};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == fixture.path &&
          call.offset == tinydb::FIRST_DATA_PAGE_ID * tinydb::PAGE_SIZE) {
        auto lock = std::lock_guard(mutex);
        page_write_reached = true;
        changed.notify_all();
      }
      return std::nullopt;
    }};
    auto checkpoint = std::thread([&] {
      checkpoint_status = fixture.manager->Checkpoint();
      {
        auto lock = std::lock_guard(mutex);
        checkpoint_done = true;
      }
      changed.notify_all();
    });
    {
      auto lock = std::unique_lock(mutex);
      changed.wait(lock, [&] { return page_write_reached; });
    }

    {
      auto lock = std::unique_lock(mutex);
      changed.wait(lock, [&] { return checkpoint_done; });
    }

    // Readers retain immutable state pointers, so neither capture, file I/O,
    // nor publication of the new checkpoint frontier waits for this reader.
    EXPECT_EQ(fixture.readers->Stats().active_readers, 1U);
    EXPECT_EQ(reader.State().checkpoint_lsn, 0U);
    EXPECT_EQ(fixture.readers->CurrentState()->checkpoint_lsn, 6U);
    checkpoint.join();
    reader = tinydb::txn::SnapshotToken{};
  }
  EXPECT_TRUE(checkpoint_status.Ok()) << checkpoint_status.ToString();
}

TEST(CheckpointManagerTest, FailedDataSyncKeepsOldFrontiersAndCanBeRetried) {
  auto fixture = Fixture("retry_data_sync");
  fixture.UsePolicy();
  fixture.Publish(7, 1, std::byte{0x37});

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Fsync && call.path == fixture.path) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto failed = fixture.manager->Checkpoint();
    EXPECT_EQ(failed.Code(), tinydb::StatusCode::IoError);
  }

  EXPECT_EQ(fixture.disk->CheckpointLsn(), 0U);
  EXPECT_EQ(fixture.readers->CurrentState()->checkpoint_lsn, 0U);
  EXPECT_FALSE(fixture.cache->DirtyPageIds().empty());
  ASSERT_TRUE(fixture.manager->Checkpoint().Ok());
  EXPECT_EQ(fixture.disk->CheckpointLsn(), 7U);
  EXPECT_EQ(fixture.readers->CurrentState()->checkpoint_lsn, 7U);
  EXPECT_TRUE(fixture.cache->DirtyPageIds().empty());
}

TEST(CheckpointManagerTest, FailedSuperblockSyncDoesNotAdvanceLiveFrontiers) {
  auto fixture = Fixture("retry_superblock_sync");
  fixture.UsePolicy();
  fixture.Publish(8, 1, std::byte{0x38});

  auto database_syncs = std::size_t{0};
  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Fsync && call.path == fixture.path && ++database_syncs == 2) {
        return tinydb::io::Fault{.error = EIO};
      }
      return std::nullopt;
    }};
    const auto failed = fixture.manager->Checkpoint();
    EXPECT_EQ(failed.Code(), tinydb::StatusCode::IoError);
  }

  // The inactive slot may contain complete bytes, but fsync did not establish
  // their durability. Live reuse and eviction therefore retain the old basis.
  EXPECT_EQ(fixture.disk->CheckpointLsn(), 0U);
  EXPECT_EQ(fixture.readers->CurrentState()->checkpoint_lsn, 0U);
  EXPECT_FALSE(fixture.cache->DirtyPageIds().empty());
  ASSERT_TRUE(fixture.manager->Checkpoint().Ok());
  EXPECT_EQ(fixture.disk->CheckpointLsn(), 8U);
  EXPECT_EQ(fixture.readers->CurrentState()->checkpoint_lsn, 8U);
}

TEST(CheckpointManagerTest, RepeatedFailureAppliesBoundedRecoverableBackpressure) {
  auto fixture = Fixture("checkpoint_backpressure");
  fixture.UsePolicy(tinydb::checkpoint::Policy{
      .wal_trigger_bytes = 1,
      .dirty_trigger_bytes = tinydb::PAGE_SIZE,
      .hard_wal_bytes = 1,
      .hard_dirty_bytes = tinydb::PAGE_SIZE,
      .failures_before_backpressure = 2,
      .maximum_age = std::chrono::seconds(0),
  });
  fixture.Publish(9, 1, std::byte{0x49});

  {
    auto hook = ScopedSyscallHook{[&](const tinydb::io::Call &call) -> std::optional<tinydb::io::Fault> {
      if (call.syscall == tinydb::io::Syscall::Pwrite && call.path == fixture.path &&
          call.offset >= tinydb::FIRST_DATA_PAGE_ID * tinydb::PAGE_SIZE) {
        return tinydb::io::Fault{.error = ENOSPC};
      }
      return std::nullopt;
    }};
    EXPECT_EQ(fixture.manager->Checkpoint().Code(), tinydb::StatusCode::IoError);
    EXPECT_TRUE(fixture.manager->WriteAdmissionStatus().Ok());
    EXPECT_EQ(fixture.manager->Checkpoint().Code(), tinydb::StatusCode::IoError);
    EXPECT_EQ(fixture.manager->WriteAdmissionStatus().Code(), tinydb::StatusCode::ResourceExhausted);
  }

  ASSERT_TRUE(fixture.manager->Checkpoint().Ok());
  EXPECT_TRUE(fixture.manager->WriteAdmissionStatus().Ok());
  EXPECT_EQ(fixture.manager->GetStats().consecutive_failures, 0U);
}
