#include <gtest/gtest.h>

#include <tinydb/page.h>
#include <tinydb/storage_engine.h>
#include <tinydb/wal.h>

#include "btree/b_plus_tree.h"
#include "btree/leaf_page_builder.h"
#include "btree/page_source.h"
#include "btree/value.h"
#include "storage/page_codec.h"
#include "util/crc32.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/*
** OVERFLOW VALUE TEST MODEL
**
** Public tests prove that large values retain ordinary transaction, cursor,
** recovery, replacement, and deletion semantics. The hostile-page model below
** bypasses the public writer and constructs individually checksummed pages. It
** verifies cross-page invariants that a page checksum alone cannot prove:
** ownership, chunk order, exact termination, logical checksum, and unique
** reachability from one leaf descriptor.
*/
namespace {

auto Pattern(std::size_t bytes, char seed = 'a') -> std::string {
  auto value = std::string(bytes, '\0');
  for (std::size_t index = 0; index < bytes; ++index) {
    value[index] = static_cast<char>(seed + static_cast<char>(index % 23));
  }
  return value;
}

class MemoryPages final : public tinydb::PageReader {
 public:
  void Put(tinydb::page_id_t page_id, std::array<char, tinydb::PAGE_SIZE> page) {
    pages_[page_id] = std::make_unique<std::array<char, tinydb::PAGE_SIZE>>(std::move(page));
  }

  auto Mutable(tinydb::page_id_t page_id) -> std::array<char, tinydb::PAGE_SIZE> & { return *pages_.at(page_id); }

  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    const auto page = pages_.find(page_id);
    if (page == pages_.end()) {
      return std::unexpected(tinydb::Status::Corruption("test page is missing"));
    }
    return tinydb::PageHandle(page->second.get(), page_id, page->second->data(), false, Release);
  }

 private:
  static void Release(void *, tinydb::page_id_t, bool) {}

  std::unordered_map<tinydb::page_id_t, std::unique_ptr<std::array<char, tinydb::PAGE_SIZE>>> pages_;
};

auto Descriptor(tinydb::page_id_t first_page, std::string_view value) -> tinydb::OverflowValueDescriptor {
  return tinydb::OverflowValueDescriptor{
      .total_value_bytes = value.size(),
      .first_page_id = first_page,
      .value_checksum = tinydb::Crc32(value.data(), value.size()),
  };
}

auto Leaf(tinydb::page_id_t page_id,
          std::vector<std::pair<std::string, tinydb::OverflowValueDescriptor>> values)
    -> std::array<char, tinydb::PAGE_SIZE> {
  auto builder = tinydb::LeafPageBuilder{};
  for (auto &[key, value] : values) {
    builder.Upsert(key, tinydb::LeafValue::Overflow(value));
  }
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  builder.Store(page.data(), page_id);
  return page;
}

auto OverflowPage(tinydb::page_id_t page_id, tinydb::page_id_t owner, std::uint32_t chunk,
                  tinydb::page_id_t next, std::string_view payload) -> std::array<char, tinydb::PAGE_SIZE> {
  return tinydb::storage::EncodeOverflowPage(page_id, 1, owner, chunk, next,
                                             std::as_bytes(std::span{payload.data(), payload.size()}))
      .value();
}

auto Integrity(MemoryPages *pages, tinydb::page_id_t high_water) -> tinydb::Status {
  return tinydb::BPlusTree::CheckIntegrity(pages, 2, high_water, {}, {});
}

TEST(OverflowIntegrityTest, RejectsDamagedMisownedReorderedAndTruncatedChains) {
  const auto full = Pattern(tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES);
  const auto tail = Pattern(17, 'd');
  const auto complete = full + tail;

  {
    auto pages = MemoryPages{};
    pages.Put(2, Leaf(2, {{"key", Descriptor(3, complete)}}));
    pages.Put(3, OverflowPage(3, 3, 0, 4, full));
    pages.Put(4, OverflowPage(4, 3, 1, tinydb::HEADER_PAGE_ID, tail));
    pages.Mutable(4).back() ^= 0x01;
    EXPECT_EQ(Integrity(&pages, 5).Code(), tinydb::StatusCode::Corruption);
  }
  {
    auto pages = MemoryPages{};
    pages.Put(2, Leaf(2, {{"key", Descriptor(3, complete)}}));
    pages.Put(3, OverflowPage(3, 4, 0, 4, full));
    pages.Put(4, OverflowPage(4, 3, 1, tinydb::HEADER_PAGE_ID, tail));
    EXPECT_EQ(Integrity(&pages, 5).Code(), tinydb::StatusCode::Corruption);
  }
  {
    auto pages = MemoryPages{};
    pages.Put(2, Leaf(2, {{"key", Descriptor(3, complete)}}));
    pages.Put(3, OverflowPage(3, 3, 1, 4, full));
    pages.Put(4, OverflowPage(4, 3, 0, tinydb::HEADER_PAGE_ID, tail));
    EXPECT_EQ(Integrity(&pages, 5).Code(), tinydb::StatusCode::Corruption);
  }
  {
    auto pages = MemoryPages{};
    pages.Put(2, Leaf(2, {{"key", Descriptor(3, complete)}}));
    pages.Put(3, OverflowPage(3, 3, 0, tinydb::HEADER_PAGE_ID, full));
    EXPECT_EQ(Integrity(&pages, 4).Code(), tinydb::StatusCode::Corruption);
  }
}

TEST(OverflowIntegrityTest, RejectsCyclesDuplicateOwnershipAndLogicalChecksumDamage) {
  const auto full = Pattern(tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES);

  {
    auto pages = MemoryPages{};
    const auto advertised = full + full + full;
    pages.Put(2, Leaf(2, {{"key", Descriptor(3, advertised)}}));
    pages.Put(3, OverflowPage(3, 3, 0, 4, full));
    pages.Put(4, OverflowPage(4, 3, 1, 3, full));
    EXPECT_EQ(Integrity(&pages, 5).Code(), tinydb::StatusCode::Corruption);
  }
  {
    auto pages = MemoryPages{};
    const auto descriptor = Descriptor(3, full);
    pages.Put(2, Leaf(2, {{"alpha", descriptor}, {"omega", descriptor}}));
    pages.Put(3, OverflowPage(3, 3, 0, tinydb::HEADER_PAGE_ID, full));
    EXPECT_EQ(Integrity(&pages, 4).Code(), tinydb::StatusCode::Corruption);
  }
  {
    auto pages = MemoryPages{};
    auto descriptor = Descriptor(3, full);
    ++descriptor.value_checksum;
    pages.Put(2, Leaf(2, {{"key", descriptor}}));
    pages.Put(3, OverflowPage(3, 3, 0, tinydb::HEADER_PAGE_ID, full));
    EXPECT_EQ(Integrity(&pages, 4).Code(), tinydb::StatusCode::Corruption);
  }
}

class OverflowEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    const auto stem = "tinydb_overflow_" + std::string(info->name()) + "_" + std::to_string(::getpid());
    path_ = std::filesystem::temp_directory_path() / (stem + ".db");
    crash_copy_ = std::filesystem::temp_directory_path() / (stem + "_copy.db");
    Remove(path_);
    Remove(crash_copy_);
  }

  void TearDown() override {
    Remove(path_);
    Remove(crash_copy_);
  }

  static void Remove(const std::filesystem::path &path) {
    std::filesystem::remove(path);
    std::filesystem::remove(tinydb::Wal::PathFor(path));
  }

  void CopyCrashImage() const {
    std::filesystem::copy_file(path_, crash_copy_);
    std::filesystem::copy_file(tinydb::Wal::PathFor(path_), tinydb::Wal::PathFor(crash_copy_));
  }

  std::filesystem::path path_;
  std::filesystem::path crash_copy_;
};

TEST_F(OverflowEngineTest, PointReadsAndCursorsCopyValuesAcrossManyPages) {
  const auto value = Pattern(9 * tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES + 137);
  {
    auto engine = tinydb::StorageEngine::Open(path_).value();
    auto write = engine.BeginWrite().value();
    ASSERT_TRUE(write.Put("large", value).Ok());
    EXPECT_EQ(write.Get("large").value(), std::optional<std::string>{value});
    ASSERT_TRUE(write.Commit());

    {
      auto read = engine.BeginRead().value();
      auto cursor = read.Scan(tinydb::KeyRange::Prefix("lar")).value();
      ASSERT_TRUE(cursor.Valid());
      EXPECT_EQ(cursor.ValueSize(), value.size());
      EXPECT_EQ(cursor.CopyValue().value(), value);
      ASSERT_TRUE(cursor.Next().Ok());
      EXPECT_FALSE(cursor.Valid());
    }

    ASSERT_TRUE(engine.Checkpoint().Ok());
    ASSERT_TRUE(engine.Close().Ok());
  }
  auto reopened = tinydb::StorageEngine::Open(path_).value();
  EXPECT_EQ(reopened.Get("large").value(), std::optional<std::string>{value});
  EXPECT_TRUE(reopened.CheckIntegrity().Ok());
}

TEST_F(OverflowEngineTest, ReplacementDeletionAndAbortRetireOnlyCommittedChains) {
  const auto first = Pattern(5 * tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES + 11, 'b');
  const auto second = Pattern(7 * tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES + 29, 'c');
  auto engine = tinydb::StorageEngine::Open(path_).value();
  ASSERT_TRUE(engine.Put("key", first).Ok());

  {
    auto write = engine.BeginWrite().value();
    ASSERT_TRUE(write.Put("key", second).Ok());
    EXPECT_EQ(write.Get("key").value(), std::optional<std::string>{second});
    write.Abort();
  }
  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{first});

  ASSERT_TRUE(engine.Put("key", "inline").Ok());
  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{"inline"});
  ASSERT_TRUE(engine.Put("key", second).Ok());
  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{second});

  {
    auto write = engine.BeginWrite().value();
    ASSERT_TRUE(write.Delete("key").Ok());
    write.Abort();
  }
  EXPECT_EQ(engine.Get("key").value(), std::optional<std::string>{second});
  ASSERT_TRUE(engine.Remove("key").Ok());
  EXPECT_EQ(engine.Get("key").value(), std::nullopt);
  EXPECT_TRUE(engine.CheckIntegrity().Ok());
}

TEST_F(OverflowEngineTest, PartialChainAllocationFailureAbortsTheWholeTransaction) {
  const auto maximum = Pattern(tinydb::MAX_VALUE_BYTES, 'f');
  auto engine = tinydb::StorageEngine::Open(path_).value();
  ASSERT_TRUE(engine.Put("baseline", "safe").Ok());

  auto write = engine.BeginWrite().value();
  ASSERT_TRUE(write.Put("first", maximum).Ok());
  // The transaction budget can hold one maximum value but not two. Failure
  // occurs after part of the second chain has been allocated privately; the
  // public write path must abort every page rather than publish a prefix.
  EXPECT_EQ(write.Put("second", maximum).Code(), tinydb::StatusCode::ResourceExhausted);
  EXPECT_EQ(write.Get("first").error().Code(), tinydb::StatusCode::Closed);

  EXPECT_EQ(engine.Get("baseline").value(), std::optional<std::string>{"safe"});
  EXPECT_EQ(engine.Get("first").value(), std::nullopt);
  EXPECT_EQ(engine.Get("second").value(), std::nullopt);
  EXPECT_TRUE(engine.CheckIntegrity().Ok());
}

TEST_F(OverflowEngineTest, CrashRecoveryReplaysCreationAndRetirementAsWholeTransactions) {
  const auto value = Pattern(6 * tinydb::storage::OVERFLOW_PAGE_PAYLOAD_BYTES + 41, 'e');
  auto engine = tinydb::StorageEngine::Open(path_).value();
  ASSERT_TRUE(engine.Put("key", value).Ok());
  CopyCrashImage();
  {
    auto recovered = tinydb::StorageEngine::Open(crash_copy_).value();
    EXPECT_EQ(recovered.Get("key").value(), std::optional<std::string>{value});
    EXPECT_TRUE(recovered.CheckIntegrity().Ok());
    ASSERT_TRUE(recovered.Close().Ok());
  }

  Remove(crash_copy_);
  ASSERT_TRUE(engine.Remove("key").Ok());
  CopyCrashImage();
  auto recovered = tinydb::StorageEngine::Open(crash_copy_).value();
  EXPECT_EQ(recovered.Get("key").value(), std::nullopt);
  EXPECT_TRUE(recovered.CheckIntegrity().Ok());
}

}  // namespace
