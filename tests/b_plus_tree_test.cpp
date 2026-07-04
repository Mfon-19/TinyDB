#include <gtest/gtest.h>
#include <tinydb/b_plus_tree.h>

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
#include "btree/page_format.h"

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

    disk_.emplace(db_path_);
    pool_.emplace(&*disk_, FRAME_COUNT);
    root_page_id_ = pool_->NewPage().page_id;  // left zeroed; the tree bootstraps it
    pool_->UnpinPage(root_page_id_, true);
    tree_.emplace(&*pool_, root_page_id_);
  }

  void TearDown() override {
    tree_.reset();
    pool_.reset();
    disk_.reset();
    std::filesystem::remove(db_path_);
  }

  void PutRow(int row, std::size_t length) {
    const auto key = RowKey(row);
    const auto value = RowValue(row, length);
    tree_->Put(key, value);
    model_[key] = value;
  }

  void RemoveRow(int row) {
    const auto key = RowKey(row);
    tree_->Remove(key);
    model_.erase(key);
  }

  // The tree and the model must agree exactly: same rows, same order.
  void ExpectMatchesModel() {
    const auto rows = tree_->Scan("", SCAN_END);
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
    const auto got = tree_->Get(key);
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
    pool_->FlushAllPages();
    pool_.reset();
    disk_.reset();

    disk_.emplace(db_path_);
    pool_.emplace(&*disk_, FRAME_COUNT);
    tree_.emplace(&*pool_, root_page_id_);
  }

  auto RootType() -> tinydb::NodeType {
    char *page = pool_->FetchPage(root_page_id_);
    const auto type = reinterpret_cast<tinydb::NodeHeader *>(page)->type;
    pool_->UnpinPage(root_page_id_, false);
    return type;
  }

  // Levels from the root down to (and including) the leaves.
  auto TreeDepth() -> int {
    int depth = 1;
    auto page_id = root_page_id_;
    for (;;) {
      char *page = pool_->FetchPage(page_id);
      const auto type = reinterpret_cast<tinydb::NodeHeader *>(page)->type;
      const auto first_child = reinterpret_cast<tinydb::InternalHeader *>(page)->first_child;
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
      char *page = pool_->FetchPage(page_id);
      const auto type = reinterpret_cast<tinydb::NodeHeader *>(page)->type;
      const auto first_child = reinterpret_cast<tinydb::InternalHeader *>(page)->first_child;
      pool_->UnpinPage(page_id, false);
      if (type == tinydb::NodeType::Leaf) {
        break;
      }
      page_id = first_child;
    }

    int length = 0;
    while (page_id != tinydb::HEADER_PAGE_ID) {
      char *page = pool_->FetchPage(page_id);
      const auto next_leaf = reinterpret_cast<tinydb::LeafHeader *>(page)->next_leaf;
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
  std::optional<tinydb::BPlusTree> tree_;
  std::map<std::string, std::string> model_;
};

using BPlusTreeDeathTest = BPlusTreeTest;

TEST_F(BPlusTreeTest, StartsEmpty) {
  EXPECT_EQ(RootType(), tinydb::NodeType::Leaf);
  EXPECT_EQ(TreeDepth(), 1);
  EXPECT_EQ(tree_->Get(RowKey(1)), std::nullopt);
  EXPECT_TRUE(tree_->Scan("", SCAN_END).empty());
  tree_->Remove(RowKey(1));  // deleting from an empty tree is a no-op
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
  EXPECT_EQ(tree_->Get(RowKey(4)), std::nullopt);
  EXPECT_EQ(tree_->Get("row-"), std::nullopt);  // prefix of real keys
}

TEST_F(BPlusTreeTest, OverwriteKeepsOneRowPerKey) {
  for (int i = 0; i < 30; ++i) {
    PutRow(i, 60);
  }

  PutRow(15, 3);    // shrink
  PutRow(15, 800);  // grow enough to force the leaf to reorganize
  PutRow(15, 0);    // empty value

  ExpectMatchesModel();
  EXPECT_EQ(tree_->Get(RowKey(15)), std::optional<std::string>{""});
}

TEST_F(BPlusTreeTest, DeleteIsIdempotent) {
  PutRow(7, 25);
  RemoveRow(7);
  ExpectMatchesModel();

  tree_->Remove(RowKey(7));  // second delete of the same key
  tree_->Remove(RowKey(8));  // delete of a key that never existed
  ExpectMatchesModel();

  PutRow(7, 40);
  ExpectMatchesModel();
}

TEST_F(BPlusTreeTest, MinimalKeysAndValues) {
  tree_->Put("", "value under empty key");
  model_[""] = "value under empty key";
  tree_->Put("k", "");
  model_["k"] = "";

  ExpectMatchesModel();
  EXPECT_EQ(tree_->Get(""), std::optional<std::string>{"value under empty key"});
  EXPECT_EQ(tree_->Get("k"), std::optional<std::string>{""});
}

TEST_F(BPlusTreeTest, ScanBoundsAreHalfOpen) {
  for (int i = 10; i < 20; ++i) {
    PutRow(i, 15);
  }

  // [start, end): the end key itself is excluded.
  const auto rows = tree_->Scan(RowKey(12), RowKey(15));
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows.front().first, RowKey(12));
  EXPECT_EQ(rows.back().first, RowKey(14));

  // Bounds that fall between keys behave like lower bounds.
  const auto between = tree_->Scan("row-000011x", "row-000013x");
  ASSERT_EQ(between.size(), 2);
  EXPECT_EQ(between.front().first, RowKey(12));
  EXPECT_EQ(between.back().first, RowKey(13));

  EXPECT_TRUE(tree_->Scan(RowKey(15), RowKey(15)).empty());  // empty range
  EXPECT_TRUE(tree_->Scan(RowKey(19), RowKey(11)).empty());  // inverted
  EXPECT_TRUE(tree_->Scan(RowKey(50), SCAN_END).empty());    // past the data
}

TEST_F(BPlusTreeTest, RootSplitKeepsRootPageId) {
  int row = 0;
  while (RootType() == tinydb::NodeType::Leaf) {
    PutRow(row, 120);
    ++row;
    ASSERT_LT(row, 100) << "root never split";
  }

  // Same root page id serves the now two-level tree.
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
    tree_->Put(key, value);
    model_[key] = value;
  }

  EXPECT_GE(TreeDepth(), 3);
  ExpectMatchesModel();

  const auto rows = tree_->Scan(RowKey(150), RowKey(160));
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
    tree_->Put(key, value);
    model_[key] = value;
  }

  ExpectMatchesModel();
}

TEST_F(BPlusTreeDeathTest, OversizedEntryAborts) {
  const auto key = RowKey(1);
  const auto value = RowValue(1, tinydb::MAX_ENTRY_BYTES - key.size() + 1);
  EXPECT_DEATH(tree_->Put(key, value), "MAX_ENTRY_BYTES");
}

TEST_F(BPlusTreeTest, DrainToEmptyCollapsesRoot) {
  for (int i = 0; i < 250; ++i) {
    PutRow(i, 150);
  }
  ASSERT_GE(TreeDepth(), 2);

  for (int i = 0; i < 250; ++i) {
    RemoveRow(i);
  }

  ExpectMatchesModel();
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
    EXPECT_EQ(tree_->Scan(start, end), want);
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

// Cells whose flags byte is nonzero are tombstones: invisible to reads and
// dropped on the next rewrite. Built by hand since the tree never writes
// them itself.
TEST_F(BPlusTreeTest, TombstoneCellsStayDead) {
  const auto [page_id, page] = pool_->NewPage();

  auto *header = reinterpret_cast<tinydb::LeafHeader *>(page);
  *header = tinydb::LeafHeader{
      .type = tinydb::NodeType::Leaf,
      .cell_count = 0,
      .free_start = sizeof(tinydb::LeafHeader),
      .free_end = static_cast<std::uint16_t>(tinydb::PAGE_SIZE),
      .next_leaf = tinydb::HEADER_PAGE_ID,
  };
  auto *slots = reinterpret_cast<std::uint16_t *>(page + sizeof(tinydb::LeafHeader));

  const auto append_cell = [&](const std::string &key, const std::string &value, std::uint8_t flags) {
    const auto cell_size = sizeof(tinydb::LeafCellHeader) + key.size() + value.size();
    const auto offset =
        static_cast<std::uint16_t>((header->free_end - cell_size) & ~std::size_t{alignof(tinydb::LeafCellHeader) - 1});
    const auto cell_header = tinydb::LeafCellHeader{
        .key_size = static_cast<std::uint16_t>(key.size()),
        .value_size = static_cast<std::uint16_t>(value.size()),
        .flags = flags,
    };
    std::memcpy(page + offset, &cell_header, sizeof(cell_header));
    std::copy_n(key.data(), key.size(), page + offset + sizeof(cell_header));
    std::copy_n(value.data(), value.size(), page + offset + sizeof(cell_header) + key.size());
    slots[header->cell_count] = offset;
    ++header->cell_count;
    header->free_start = static_cast<std::uint16_t>(header->free_start + sizeof(std::uint16_t));
    header->free_end = offset;
  };

  append_cell("live", "here", 0);
  append_cell("zombie", "gone", 1);
  pool_->UnpinPage(page_id, true);

  tinydb::BPlusTree tree(&*pool_, page_id);
  EXPECT_EQ(tree.Get("live"), std::optional<std::string>{"here"});
  EXPECT_EQ(tree.Get("zombie"), std::nullopt);

  // Any mutation rewrites the page; the tombstone must not resurrect.
  tree.Put("new", "row");
  const auto rows = tree.Scan("", SCAN_END);
  const auto want = std::vector<std::pair<std::string, std::string>>{
      {"live", "here"},
      {"new", "row"},
  };
  EXPECT_EQ(rows, want);
}

}  // namespace
