#include <gtest/gtest.h>

#include <tinydb/disk_manager.h>

#include "btree/leaf_page_builder.h"
#include "cache/committed_page_cache.h"
#include "cache/committed_page_source.h"
#include "txn/database_state.h"
#include "txn/read_snapshot.h"
#include "txn/reader_gate.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
** These integration tests join ReaderGate, committed cache, PageReader, and
** B+ tree cursor behavior. They verify that a captured root remains stable,
** cursor admission outlives its wrapper, and publication cannot replace old
** page versions until the final cursor lease is released.
*/
namespace {

using namespace std::chrono_literals;

auto TestPath(std::string_view name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_read_snapshot_" + std::string(name) + ".db");
}

auto Leaf(tinydb::page_id_t page_id, std::initializer_list<std::pair<std::string_view, std::string_view>> rows)
    -> std::unique_ptr<tinydb::cache::PageBytes> {
  auto builder = tinydb::LeafPageBuilder{};
  for (const auto &[key, value] : rows) {
    builder.Upsert(key, value);
  }
  auto page = std::make_unique<tinydb::cache::PageBytes>();
  builder.Store(page->data(), page_id);
  return page;
}

auto State(std::uint64_t transaction_id,
           tinydb::page_id_t root_page_id) -> std::shared_ptr<const tinydb::txn::DatabaseState> {
  return std::make_shared<const tinydb::txn::DatabaseState>(tinydb::txn::DatabaseState{
      .root_page_id = root_page_id,
      .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
      .high_water_page_id = root_page_id + 1,
      .transaction_id = transaction_id,
      .visible_lsn = transaction_id,
      .checkpoint_lsn = transaction_id,
  });
}

struct SnapshotFixture {
  SnapshotFixture() : path(TestPath("integration")) {
    std::filesystem::remove(path);
    disk = std::make_unique<tinydb::DiskManager>(tinydb::DiskManager::Open(path).value());
    root = disk->HighWaterPageId();
    EXPECT_TRUE(disk->EnsurePageCount(root + 1).Ok());
    auto initial = Leaf(root, {{"alpha", "one"}, {"bravo", "two"}});
    EXPECT_TRUE(disk->WriteCheckpointPage(root, initial->data(), root + 1).Ok());
    EXPECT_TRUE(disk->Sync().Ok());
    EXPECT_TRUE(disk->CommitCheckpoint(root, disk->GetAllocatorRootPageId(), root + 1, 1, 1).Ok());
    cache = std::make_unique<tinydb::cache::CommittedPageCache>(disk.get(), 4 * tinydb::PAGE_SIZE, 1);
    pages = std::make_unique<tinydb::cache::CommittedPageSource>(cache.get());
    gate = std::make_unique<tinydb::txn::ReaderGate>(State(1, root));
  }

  ~SnapshotFixture() {
    gate.reset();
    pages.reset();
    cache.reset();
    disk.reset();
    std::filesystem::remove(path);
  }

  std::filesystem::path path;
  tinydb::page_id_t root{tinydb::HEADER_PAGE_ID};
  std::unique_ptr<tinydb::DiskManager> disk;
  std::unique_ptr<tinydb::cache::CommittedPageCache> cache;
  std::unique_ptr<tinydb::cache::CommittedPageSource> pages;
  std::unique_ptr<tinydb::txn::ReaderGate> gate;
};

TEST(ReadSnapshotTest, PointReadsAndCursorUseTheCapturedRoot) {
  auto fixture = SnapshotFixture{};
  auto snapshot = tinydb::txn::ReadSnapshot::Begin(fixture.gate.get(), fixture.pages.get());

  EXPECT_EQ(snapshot.State().transaction_id, 1U);
  EXPECT_EQ(snapshot.Get("alpha").value(), std::optional<std::string>{"one"});
  EXPECT_EQ(snapshot.Get("absent").value(), std::nullopt);

  auto cursor = snapshot.Seek("b").value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "bravo");
  EXPECT_EQ(cursor.CopyValue().value(), "two");
  EXPECT_TRUE(cursor.Next().Ok());
  EXPECT_FALSE(cursor.Valid());
}

TEST(ReadSnapshotTest, CursorRetainsAdmissionAfterSnapshotWrapperDies) {
  auto fixture = SnapshotFixture{};
  auto cursor = std::optional<tinydb::txn::SnapshotCursor>{[&] {
    auto snapshot = tinydb::txn::ReadSnapshot::Begin(fixture.gate.get(), fixture.pages.get());
    return snapshot.Seek("").value();
  }()};

  EXPECT_EQ(fixture.gate->Stats().active_readers, 1U);
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->Key(), "alpha");
  cursor.reset();
  EXPECT_EQ(fixture.gate->Stats().active_readers, 0U);
}

TEST(ReadSnapshotTest, PublicationWaitsUntilCursorReleasesOldPageVersion) {
  auto fixture = SnapshotFixture{};
  auto old_cursor = std::optional<tinydb::txn::SnapshotCursor>{[&] {
    auto old_snapshot = tinydb::txn::ReadSnapshot::Begin(fixture.gate.get(), fixture.pages.get());
    return old_snapshot.Seek("").value();
  }()};

  auto published = std::atomic<bool>{false};
  auto publisher = std::thread([&] {
    auto replacement = Leaf(fixture.root, {{"alpha", "new"}, {"charlie", "three"}});
    auto images = std::vector<tinydb::cache::CommittedPageImage>{};
    images.push_back(tinydb::cache::CommittedPageImage{
        .page_id = fixture.root,
        .page_lsn = 0,
        .transaction_id = 2,
        .bytes = std::move(replacement),
    });
    auto plan = fixture.cache->PreparePublication(std::move(images), {}, fixture.root + 1).value();
    auto publication = fixture.gate->BeginPublication();
    fixture.cache->Publish(std::move(plan));
    publication.Publish(State(2, fixture.root));
    published.store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!fixture.gate->Stats().publication_pending && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(fixture.gate->Stats().publication_pending);
  EXPECT_FALSE(published.load(std::memory_order_acquire));
  EXPECT_EQ(old_cursor->CopyValue().value(), "one");

  old_cursor.reset();
  publisher.join();
  ASSERT_TRUE(published.load(std::memory_order_acquire));

  auto current = tinydb::txn::ReadSnapshot::Begin(fixture.gate.get(), fixture.pages.get());
  EXPECT_EQ(current.State().transaction_id, 2U);
  EXPECT_EQ(current.Get("alpha").value(), std::optional<std::string>{"new"});
  EXPECT_EQ(current.Get("bravo").value(), std::nullopt);
  EXPECT_EQ(current.Get("charlie").value(), std::optional<std::string>{"three"});
}

}  // namespace
