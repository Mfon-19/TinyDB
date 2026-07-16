#include <gtest/gtest.h>
#include "btree/b_plus_tree.h"
#include <tinydb/buffer_pool.h>
#include <tinydb/disk_manager.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

// The suite asserts on tree structure by inspecting raw page headers, so it
// reaches into the library's private on-disk format header.
#include "btree/buffer_pool_page_source.h"
#include "btree/page_format.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"

namespace {

auto RowKey(int row) -> std::string {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "row-%06d", row);
  return std::string{buffer};
}

// Deterministic content: digits cycling from an offset, so two different
// rows (or lengths) never collide by accident.
auto RowValue(int row, std::size_t length) -> std::string {
  auto value = std::string(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    value[i] = static_cast<char>('0' + (static_cast<std::size_t>(row) + i) % 10);
  }
  return value;
}

// Scan end bound past every key this suite generates.
constexpr const char *SCAN_END = "\x7f";

class BPlusTreeTest : public ::testing::Test {
 protected:
  static constexpr std::size_t FRAME_COUNT = 16;

  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    db_path_ = std::filesystem::temp_directory_path() /
               ("tinydb_bpt_" + std::string(info->name()) + "_" + std::to_string(::getpid()) + ".db");
    std::filesystem::remove(db_path_);

    disk_.emplace(tinydb::DiskManager::Open(db_path_).value());
    pool_.emplace(&*disk_, FRAME_COUNT);
    pages_.emplace(&*pool_);
    root_page_id_ = pool_->NewPage().value().page_id;  // left zeroed; the tree bootstraps it
    pool_->UnpinPage(root_page_id_, true);
    tree_.emplace(tinydb::BPlusTree::Open(&*pages_, root_page_id_).value());
  }

  void TearDown() override {
    tree_.reset();
    pages_.reset();
    pool_.reset();
    disk_.reset();
    std::filesystem::remove(db_path_);
  }

  void PutRow(int row, std::size_t length) {
    const auto key = RowKey(row);
    const auto value = RowValue(row, length);
    ASSERT_TRUE(tree_->Put(key, value).Ok());
    PublishRoot();
    model_[key] = value;
  }

  void RemoveRow(int row) {
    const auto key = RowKey(row);
    ASSERT_TRUE(tree_->Remove(key).Ok());
    PublishRoot();
    model_.erase(key);
  }

  // The tree and the model must agree exactly: same rows, same order.
  void ExpectMatchesModel() {
    const auto rows = tree_->Scan("", SCAN_END).value();
    ASSERT_EQ(rows.size(), model_.size());
    auto it = model_.begin();
    for (const auto &[key, value] : rows) {
      EXPECT_EQ(key, it->first);
      EXPECT_EQ(value, it->second);
      ++it;
    }
  }

  void ExpectGetMatchesModel(int row) {
    const auto key = RowKey(row);
    const auto got = tree_->Get(key).value();
    const auto want = model_.find(key);
    if (want == model_.end()) {
      EXPECT_EQ(got, std::nullopt) << key;
    } else {
      EXPECT_EQ(got, std::optional<std::string>{want->second}) << key;
    }
  }

  // Flushes everything, tears the stack down, and rebuilds it from the file.
  void ReopenDatabase() {
    tree_.reset();
    ASSERT_TRUE(pool_->FlushAllPages().Ok());
    ASSERT_TRUE(disk_->Checkpoint().Ok());  // metadata reaches the file at a checkpoint
    pages_.reset();
    pool_.reset();
    disk_.reset();

    disk_.emplace(tinydb::DiskManager::Open(db_path_).value());
    pool_.emplace(&*disk_, FRAME_COUNT);
    pages_.emplace(&*pool_);
    root_page_id_ = disk_->GetRootPageId();
    tree_.emplace(tinydb::BPlusTree::Open(&*pages_, root_page_id_).value());
  }

  // BPlusTree owns the logical root while it mutates. A real transaction
  // publishes this value atomically with page images; this white-box fixture
  // mirrors that handoff into DiskManager after each successful mutation.
  void PublishRoot() {
    if (root_page_id_ != tree_->RootPageId()) {
      root_page_id_ = tree_->RootPageId();
      disk_->SetRootPageId(root_page_id_);
    }
  }

  auto RootType() -> tinydb::NodeType {
    char *page = pool_->FetchPage(root_page_id_).value();
    const auto type = static_cast<tinydb::NodeType>(tinydb::RawNodeType(page));
    pool_->UnpinPage(root_page_id_, false);
    return type;
  }

  // Levels from the root down to (and including) the leaves.
  auto TreeDepth() -> int {
    int depth = 1;
    auto page_id = root_page_id_;
    for (;;) {
      char *page = pool_->FetchPage(page_id).value();
      const auto type = static_cast<tinydb::NodeType>(tinydb::RawNodeType(page));
      const auto first_child = tinydb::storage::GetLittleEndian<tinydb::page_id_t>(
                                   std::as_bytes(std::span{page, tinydb::PAGE_SIZE}), tinydb::node_page_offset::LINK)
                                   .value();
      pool_->UnpinPage(page_id, false);
      if (type == tinydb::NodeType::Leaf) {
        return depth;
      }
      ++depth;
      page_id = first_child;
    }
  }

  // Length of the next_leaf chain starting at the leftmost leaf.
  auto LeafChainLength() -> int {
    auto page_id = root_page_id_;
    for (;;) {
      char *page = pool_->FetchPage(page_id).value();
      const auto type = static_cast<tinydb::NodeType>(tinydb::RawNodeType(page));
      const auto first_child = tinydb::storage::GetLittleEndian<tinydb::page_id_t>(
                                   std::as_bytes(std::span{page, tinydb::PAGE_SIZE}), tinydb::node_page_offset::LINK)
                                   .value();
      pool_->UnpinPage(page_id, false);
      if (type == tinydb::NodeType::Leaf) {
        break;
      }
      page_id = first_child;
    }

    int length = 0;
    while (page_id != tinydb::HEADER_PAGE_ID) {
      char *page = pool_->FetchPage(page_id).value();
      const auto next_leaf = tinydb::storage::GetLittleEndian<tinydb::page_id_t>(
                                 std::as_bytes(std::span{page, tinydb::PAGE_SIZE}), tinydb::node_page_offset::LINK)
                                 .value();
      pool_->UnpinPage(page_id, false);
      page_id = next_leaf;
      ++length;
    }
    return length;
  }

  std::filesystem::path db_path_;
  tinydb::page_id_t root_page_id_{tinydb::HEADER_PAGE_ID};
  std::optional<tinydb::DiskManager> disk_;
  std::optional<tinydb::BufferPool> pool_;
  std::optional<tinydb::BufferPoolPageSource> pages_;
  std::optional<tinydb::BPlusTree> tree_;
  std::map<std::string, std::string> model_;
};

using BPlusTreeDeathTest = BPlusTreeTest;

TEST_F(BPlusTreeTest, StartsEmpty) {
  EXPECT_EQ(RootType(), tinydb::NodeType::Leaf);
  EXPECT_EQ(TreeDepth(), 1);
  EXPECT_EQ(tree_->Get(RowKey(1)).value(), std::nullopt);
  EXPECT_TRUE(tree_->Scan("", SCAN_END).value().empty());
  EXPECT_TRUE(tree_->Remove(RowKey(1)).Ok());  // deleting from an empty tree is a no-op
  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, InsertAndLookup) {
  PutRow(3, 10);
  PutRow(1, 20);
  PutRow(2, 30);

  ExpectMatchesModel();
  ExpectGetMatchesModel(1);
  ExpectGetMatchesModel(2);
  ExpectGetMatchesModel(3);
  EXPECT_EQ(tree_->Get(RowKey(4)).value(), std::nullopt);
  EXPECT_EQ(tree_->Get("row-").value(), std::nullopt);  // prefix of real keys
}

TEST_F(BPlusTreeTest, OverwriteKeepsOneRowPerKey) {
  for (int i = 0; i < 30; ++i) {
    PutRow(i, 60);
  }

  PutRow(15, 3);    // shrink
  PutRow(15, 800);  // grow enough to force the leaf to reorganize
  PutRow(15, 0);    // empty value

  ExpectMatchesModel();
  EXPECT_EQ(tree_->Get(RowKey(15)).value(), std::optional<std::string>{""});
}

TEST_F(BPlusTreeTest, DeleteIsIdempotent) {
  PutRow(7, 25);
  RemoveRow(7);
  ExpectMatchesModel();

  EXPECT_TRUE(tree_->Remove(RowKey(7)).Ok());  // second delete of the same key
  EXPECT_TRUE(tree_->Remove(RowKey(8)).Ok());  // delete of a key that never existed
  ExpectMatchesModel();

  PutRow(7, 40);
  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, MinimalKeysAndValues) {
  ASSERT_TRUE(tree_->Put("", "value under empty key").Ok());
  model_[""] = "value under empty key";
  ASSERT_TRUE(tree_->Put("k", "").Ok());
  model_["k"] = "";

  ExpectMatchesModel();
  EXPECT_EQ(tree_->Get("").value(), std::optional<std::string>{"value under empty key"});
  EXPECT_EQ(tree_->Get("k").value(), std::optional<std::string>{""});
}

TEST_F(BPlusTreeTest, ScanBoundsAreHalfOpen) {
  for (int i = 10; i < 20; ++i) {
    PutRow(i, 15);
  }

  // [start, end): the end key itself is excluded.
  const auto rows = tree_->Scan(RowKey(12), RowKey(15)).value();
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows.front().first, RowKey(12));
  EXPECT_EQ(rows.back().first, RowKey(14));

  // Bounds that fall between keys behave like lower bounds.
  const auto between = tree_->Scan("row-000011x", "row-000013x").value();
  ASSERT_EQ(between.size(), 2);
  EXPECT_EQ(between.front().first, RowKey(12));
  EXPECT_EQ(between.back().first, RowKey(13));

  EXPECT_TRUE(tree_->Scan(RowKey(15), RowKey(15)).value().empty());  // empty range
  EXPECT_TRUE(tree_->Scan(RowKey(19), RowKey(11)).value().empty());  // inverted
  EXPECT_TRUE(tree_->Scan(RowKey(50), SCAN_END).value().empty());    // past the data
}

TEST_F(BPlusTreeTest, RootSplitAllocatesNewRootPage) {
  const auto original_root = root_page_id_;
  int row = 0;
  while (RootType() == tinydb::NodeType::Leaf) {
    PutRow(row, 120);
    ++row;
    ASSERT_LT(row, 100) << "root never split";
  }

  // The original leaf remains the left half; a new ordinary internal page is
  // published as root instead of copying both halves around a permanent id.
  EXPECT_NE(root_page_id_, original_root);
  EXPECT_EQ(TreeDepth(), 2);
  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, GrowsThreeLevels) {
  // Wide keys make fat separators, so internal nodes fill up quickly and
  // the root must split a second time.
  const auto padding = std::string(120, 'p');
  for (int i = 0; i < 400; ++i) {
    const auto key = RowKey(i) + padding;
    const auto value = RowValue(i, 300);
    ASSERT_TRUE(tree_->Put(key, value).Ok());
    PublishRoot();
    model_[key] = value;
  }

  EXPECT_GE(TreeDepth(), 3);
  ExpectMatchesModel();

  const auto rows = tree_->Scan(RowKey(150), RowKey(160)).value();
  ASSERT_EQ(rows.size(), 10);
  EXPECT_EQ(rows.front().first, RowKey(150) + padding);
  EXPECT_EQ(rows.back().first, RowKey(159) + padding);
}

TEST_F(BPlusTreeTest, AscendingFillPacksLeavesDensely) {
  // Each row costs ~118 bytes in a leaf (~34 rows per 4080 usable bytes),
  // so 400 rows need at least 12 leaves. Ascending inserts should stay
  // near that floor; mid-point splits would leave ~24 half-full leaves.
  for (int i = 0; i < 400; ++i) {
    PutRow(i, 96);
  }

  ExpectMatchesModel();
  EXPECT_LE(LeafChainLength(), 15);
}

TEST_F(BPlusTreeTest, DescendingFill) {
  for (int i = 249; i >= 0; --i) {
    PutRow(i, 96);
  }

  ExpectMatchesModel();
  EXPECT_GE(TreeDepth(), 2);
}

TEST_F(BPlusTreeTest, MaxSizedEntries) {
  // key + value lands exactly on MAX_ENTRY_BYTES; a page holds only a few
  // of these, so splits happen with maximum-size records and separators.
  for (int i = 0; i < 12; ++i) {
    const auto key = RowKey(i);
    const auto value = RowValue(i, tinydb::MAX_ENTRY_BYTES - key.size());
    ASSERT_TRUE(tree_->Put(key, value).Ok());
    PublishRoot();
    model_[key] = value;
  }

  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, CyclicLeafChainReturnsCorruption) {
  ASSERT_TRUE(tree_->Put("a", "1").Ok());
  ASSERT_TRUE(tree_->Put("b", "2").Ok());

  // Corrupt the root leaf's next-leaf link to point back at itself: the
  // shape a damaged page could hand a Scan, which must return a library error
  // rather than terminate the embedding process or loop forever.
  char *page = pool_->FetchPage(root_page_id_).value();
  auto page_bytes = std::as_writable_bytes(std::span{page, tinydb::PAGE_SIZE});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(page_bytes, tinydb::node_page_offset::LINK, root_page_id_));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(page_bytes).Ok());
  pool_->UnpinPage(root_page_id_, true);

  const auto rows = tree_->Scan("a", SCAN_END);
  ASSERT_FALSE(rows.has_value());
  EXPECT_EQ(rows.error().Code(), tinydb::StatusCode::Corruption);
}

TEST_F(BPlusTreeDeathTest, OversizedEntryAborts) {
  const auto key = RowKey(1);
  const auto value = RowValue(1, tinydb::MAX_ENTRY_BYTES - key.size() + 1);
  EXPECT_DEATH(static_cast<void>(tree_->Put(key, value)), "MAX_ENTRY_BYTES");
}

TEST_F(BPlusTreeTest, DrainToEmptyCollapsesRoot) {
  for (int i = 0; i < 250; ++i) {
    PutRow(i, 150);
  }
  ASSERT_GE(TreeDepth(), 2);
  const auto expanded_root = root_page_id_;

  for (int i = 0; i < 250; ++i) {
    RemoveRow(i);
  }

  ExpectMatchesModel();
  EXPECT_NE(root_page_id_, expanded_root);
  EXPECT_EQ(TreeDepth(), 1);
  EXPECT_EQ(RootType(), tinydb::NodeType::Leaf);

  // The collapsed tree is still fully functional.
  for (int i = 500; i < 520; ++i) {
    PutRow(i, 30);
  }
  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, InterleavedChurn) {
  for (int i = 0; i < 300; ++i) {
    PutRow(i, (static_cast<std::size_t>(i) * 7) % 200 + 1);
  }
  ExpectMatchesModel();

  for (int i = 0; i < 300; i += 3) {  // punch holes everywhere
    RemoveRow(i);
  }
  ExpectMatchesModel();

  for (int i = 1; i < 300; i += 5) {  // resize survivors
    PutRow(i, 250);
  }
  ExpectMatchesModel();

  for (int i = 100; i < 180; ++i) {  // hollow out a contiguous region
    RemoveRow(i);
  }
  ExpectMatchesModel();

  for (int i = 100; i < 180; i += 2) {  // refill half of it
    PutRow(i, 64);
  }
  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, FuzzAgainstModel) {
  auto rng = std::mt19937{20260702U};
  auto row_dist = std::uniform_int_distribution<int>{0, 249};
  auto length_dist = std::uniform_int_distribution<int>{0, 280};
  auto action_dist = std::uniform_int_distribution<int>{0, 99};

  for (int op = 0; op < 2000; ++op) {
    const int row = row_dist(rng);
    const int action = action_dist(rng);

    if (action < 55) {
      PutRow(row, static_cast<std::size_t>(length_dist(rng)));
    } else if (action < 90) {
      RemoveRow(row);
    } else {
      ExpectGetMatchesModel(row);
    }

    if (op % 100 == 0) {
      ExpectMatchesModel();
    }
  }
  ExpectMatchesModel();

  // Random half-open range scans against the model.
  for (int i = 0; i < 50; ++i) {
    auto lo = row_dist(rng);
    auto hi = row_dist(rng);
    if (hi < lo) {
      std::swap(lo, hi);
    }
    const auto start = RowKey(lo);
    const auto end = RowKey(hi);

    auto want = std::vector<std::pair<std::string, std::string>>{};
    for (auto it = model_.lower_bound(start); it != model_.end() && it->first < end; ++it) {
      want.emplace_back(it->first, it->second);
    }
    EXPECT_EQ(tree_->Scan(start, end).value(), want);
  }
}

TEST_F(BPlusTreeTest, SurvivesReopen) {
  for (int i = 0; i < 160; ++i) {
    PutRow(i, 40 + (static_cast<std::size_t>(i) * 11) % 150);
  }
  for (int i = 0; i < 160; i += 4) {
    RemoveRow(i);
  }

  ReopenDatabase();
  ExpectMatchesModel();

  // A reopened tree keeps accepting mutations, across another reopen.
  for (int i = 300; i < 360; ++i) {
    PutRow(i, 75);
  }
  RemoveRow(301);

  ReopenDatabase();
  ExpectMatchesModel();
}

}  // namespace
