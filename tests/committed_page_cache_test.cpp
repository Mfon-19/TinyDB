#include <gtest/gtest.h>

#include <tinydb/disk_manager.h>

#include "cache/committed_page_cache.h"
#include "storage/page_codec.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
** Cache tests distinguish three properties that are easy to conflate:
** immutable version lifetime, eviction eligibility, and LRU policy. Encoded
** overflow pages provide checksummed marker bytes, so every assertion reads a
** valid persistent image rather than inspecting an ad-hoc test buffer.
*/
namespace {

using tinydb::cache::CommittedPageCache;
using tinydb::cache::CommittedPageImage;
using tinydb::cache::PageBytes;

auto TestPath(std::string_view name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_committed_cache_" + std::string(name) + ".db");
}

auto EncodedPage(tinydb::page_id_t page_id, std::uint64_t lsn, tinydb::page_id_t marker) -> std::unique_ptr<PageBytes> {
  const auto payload =
      std::array<std::byte, sizeof(tinydb::page_id_t)>{std::byte{static_cast<unsigned char>(marker & 0xffU)}};
  auto encoded = tinydb::storage::EncodeOverflowPage(page_id, lsn, payload.size(), tinydb::HEADER_PAGE_ID, payload);
  EXPECT_TRUE(encoded.has_value());
  return std::make_unique<PageBytes>(std::move(*encoded));
}

auto Marker(const tinydb::cache::PageGuard &page) -> tinydb::page_id_t {
  const auto decoded = tinydb::storage::DecodeOverflowPage(std::as_bytes(page.Data()), page.Id());
  EXPECT_TRUE(decoded.has_value());
  return std::to_integer<tinydb::page_id_t>(decoded->payload.front());
}

struct CacheFixture {
  explicit CacheFixture(std::string_view name) : path(TestPath(name)) {
    std::filesystem::remove(path);
    disk = std::make_unique<tinydb::DiskManager>(tinydb::DiskManager::Open(path).value());
  }

  ~CacheFixture() {
    disk.reset();
    std::filesystem::remove(path);
  }

  auto AddDiskPage(tinydb::page_id_t marker, std::uint64_t lsn = 0) -> tinydb::page_id_t {
    const auto page_id = disk->HighWaterPageId();
    disk->AdoptState(disk->GetRootPageId(), disk->GetAllocatorRootPageId(), page_id + 1, disk->TransactionId(),
                     disk->CheckpointLsn());
    EXPECT_TRUE(disk->EnsurePageCount(page_id + 1).Ok());
    const auto page = EncodedPage(page_id, lsn, marker);
    EXPECT_TRUE(disk->WritePage(page_id, page->data()).Ok());
    return page_id;
  }

  std::filesystem::path path;
  std::unique_ptr<tinydb::DiskManager> disk;
};

TEST(CommittedPageCacheTest, ConcurrentGuardsShareImmutableBytes) {
  auto fixture = CacheFixture("concurrent_readers");
  const auto page_id = fixture.AddDiskPage(tinydb::HEADER_PAGE_ID);
  auto cache = CommittedPageCache(fixture.disk.get(), 2 * tinydb::PAGE_SIZE, 0);
  ASSERT_TRUE(cache.Read(page_id).has_value());  // Warm the page before racing hits.

  auto failures = std::atomic<std::size_t>{0};
  auto readers = std::vector<std::thread>{};
  for (auto index = 0; index < 8; ++index) {
    readers.emplace_back([&] {
      for (auto read = 0; read < 500; ++read) {
        auto page = cache.Read(page_id);
        if (!page || page->Id() != page_id || Marker(*page) != tinydb::HEADER_PAGE_ID) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &reader : readers) {
    reader.join();
  }

  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(cache.Stats().resident_pages, 1U);
  EXPECT_EQ(cache.Stats().pinned_pages, 0U);
}

TEST(CommittedPageCacheTest, LruEvictionSkipsPinnedFrames) {
  auto fixture = CacheFixture("pinned_lru");
  const auto first = fixture.AddDiskPage(tinydb::HEADER_PAGE_ID);
  const auto second = fixture.AddDiskPage(first);
  const auto third = fixture.AddDiskPage(second);
  auto cache = CommittedPageCache(fixture.disk.get(), 2 * tinydb::PAGE_SIZE, 0);

  auto pinned = cache.Read(first).value();
  ASSERT_TRUE(cache.Read(second).has_value());
  ASSERT_TRUE(cache.Read(third).has_value());

  EXPECT_EQ(Marker(pinned), tinydb::HEADER_PAGE_ID);
  EXPECT_EQ(cache.Stats().resident_pages, 2U);
  EXPECT_EQ(cache.Stats().pinned_pages, 1U);
}

TEST(CommittedPageCacheTest, CacheHitsRefreshTheEvictionOrder) {
  auto fixture = CacheFixture("lru_reference");
  const auto first = fixture.AddDiskPage(tinydb::HEADER_PAGE_ID);
  const auto second = fixture.AddDiskPage(first);
  const auto third = fixture.AddDiskPage(second);
  auto cache = CommittedPageCache(fixture.disk.get(), 2 * tinydb::PAGE_SIZE, 0);

  ASSERT_TRUE(cache.Read(first).has_value());
  ASSERT_TRUE(cache.Read(second).has_value());
  ASSERT_TRUE(cache.Read(first).has_value());  // first is now newer than second.

  // Change the file copy after caching. A later read reveals whether the old
  // immutable frame survived or had to be loaded again.
  const auto changed_first = EncodedPage(first, 1, third);
  const auto changed_second = EncodedPage(second, 1, third);
  ASSERT_TRUE(fixture.disk->WritePage(first, changed_first->data()).Ok());
  ASSERT_TRUE(fixture.disk->WritePage(second, changed_second->data()).Ok());

  ASSERT_TRUE(cache.Read(third).has_value());  // Evicts least-recently-used second.
  auto retained_first = cache.Read(first).value();
  auto reloaded_second = cache.Read(second).value();
  EXPECT_EQ(Marker(retained_first), tinydb::HEADER_PAGE_ID);
  EXPECT_EQ(Marker(reloaded_second), third);
}

TEST(CommittedPageCacheTest, UncheckpointedPagesAreNotEvicted) {
  auto fixture = CacheFixture("dirty_retention");
  const auto dirty_id = fixture.AddDiskPage(tinydb::HEADER_PAGE_ID);
  const auto other_id = fixture.AddDiskPage(dirty_id);
  auto cache = CommittedPageCache(fixture.disk.get(), tinydb::PAGE_SIZE, 0);

  ASSERT_TRUE(cache
                  .Install(CommittedPageImage{
                      .page_id = dirty_id,
                      .page_lsn = 5,
                      .transaction_id = 1,
                      .bytes = EncodedPage(dirty_id, 5, other_id),
                  })
                  .Ok());
  EXPECT_EQ(cache.DirtyPageIds(), std::vector<tinydb::page_id_t>{dirty_id});

  const auto blocked = cache.Read(other_id);
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(blocked.error().Code(), tinydb::StatusCode::ResourceExhausted);

  cache.MarkCheckpointed(5);
  EXPECT_TRUE(cache.DirtyPageIds().empty());
  ASSERT_TRUE(cache.Read(other_id).has_value());
  EXPECT_EQ(cache.Stats().resident_pages, 1U);
}

TEST(CommittedPageCacheTest, ReplacingAPageDoesNotMutateAnExistingGuard) {
  auto fixture = CacheFixture("immutable_replacement");
  const auto page_id = fixture.AddDiskPage(tinydb::HEADER_PAGE_ID);
  auto cache = CommittedPageCache(fixture.disk.get(), 2 * tinydb::PAGE_SIZE, 0);
  auto old = cache.Read(page_id).value();

  ASSERT_TRUE(cache
                  .Install(CommittedPageImage{
                      .page_id = page_id,
                      .page_lsn = 9,
                      .transaction_id = 3,
                      .bytes = EncodedPage(page_id, 9, page_id),
                  })
                  .Ok());
  auto latest = cache.Read(page_id).value();

  EXPECT_EQ(Marker(old), tinydb::HEADER_PAGE_ID);
  EXPECT_EQ(old.PageLsn(), 0U);
  EXPECT_EQ(Marker(latest), page_id);
  EXPECT_EQ(latest.PageLsn(), 9U);
  EXPECT_EQ(latest.TransactionId(), 3U);
}

TEST(CommittedPageCacheTest, RejectsInvalidOrRegressingCommittedImages) {
  auto fixture = CacheFixture("bad_install");
  const auto page_id = fixture.AddDiskPage(tinydb::HEADER_PAGE_ID);
  auto cache = CommittedPageCache(fixture.disk.get(), 2 * tinydb::PAGE_SIZE, 0);

  ASSERT_TRUE(cache
                  .Install(CommittedPageImage{
                      .page_id = page_id,
                      .page_lsn = 10,
                      .transaction_id = 2,
                      .bytes = EncodedPage(page_id, 10, tinydb::HEADER_PAGE_ID),
                  })
                  .Ok());
  const auto regressing = cache.Install(CommittedPageImage{
      .page_id = page_id,
      .page_lsn = 8,
      .transaction_id = 1,
      .bytes = EncodedPage(page_id, 8, tinydb::HEADER_PAGE_ID),
  });
  EXPECT_EQ(regressing.Code(), tinydb::StatusCode::InvalidArgument);

  auto mismatched = EncodedPage(page_id, 11, tinydb::HEADER_PAGE_ID);
  const auto mismatch = cache.Install(CommittedPageImage{
      .page_id = page_id,
      .page_lsn = 12,
      .transaction_id = 3,
      .bytes = std::move(mismatched),
  });
  EXPECT_EQ(mismatch.Code(), tinydb::StatusCode::InvalidArgument);
}

}  // namespace
