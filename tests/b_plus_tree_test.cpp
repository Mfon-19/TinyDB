#include <gtest/gtest.h>
#include <tinydb/b_plus_tree.h>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static auto TestPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tinydb_btree_" + name + "_" + std::to_string(::getpid()) + ".db");
}

static auto NewRootLeaf(tinydb::BufferPool *buffer_pool) -> tinydb::page_id_t {
  tinydb::page_id_t root_page_id = 0;
  char *page = buffer_pool->NewPage(&root_page_id);
  auto *header = reinterpret_cast<tinydb::LeafHeader *>(page);

  *header = tinydb::LeafHeader{
      .type = tinydb::NodeType::Leaf,
      .cell_count = 0,
      .free_start = sizeof(tinydb::LeafHeader),
      .free_end = static_cast<std::uint16_t>(tinydb::PAGE_SIZE),
      .next_leaf = tinydb::HEADER_PAGE_ID,
  };

  buffer_pool->UnpinPage(root_page_id, true);
  return root_page_id;
}

static auto TestKey(int value) -> std::string {
  auto key_stream = std::ostringstream{};
  key_stream << "key_" << std::setw(4) << std::setfill('0') << value;
  return key_stream.str();
}

static auto TestValue(int seed, std::size_t size) -> std::string {
  auto value = std::string(size, static_cast<char>('a' + (seed % 26)));
  return value;
}

static void CheckTree(tinydb::BPlusTree *tree,
                      const std::map<std::string, std::string> &expected_rows) {
  for (const auto &[key, value] : expected_rows) {
    EXPECT_EQ(tree->Get(key), std::optional<std::string>{value});
  }

  auto expected_scan = std::vector<std::pair<std::string, std::string>>{};
  for (const auto &[key, value] : expected_rows) {
    expected_scan.emplace_back(key, value);
  }

  auto end_key = std::string(1, '\x7f');
  if (!expected_rows.empty()) {
    end_key = expected_rows.rbegin()->first;
    end_key.push_back('\x7f');
  }

  EXPECT_EQ(tree->Scan("", end_key), expected_scan);
}

TEST(BPlusTreeTest, PutGet) {
  const auto path = TestPath("put_get");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 3);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    EXPECT_EQ(tree.Get("missing"), std::nullopt);

    tree.Put("banana", "yellow");
    tree.Put("apple", "red");

    auto apple = tree.Get("apple");
    EXPECT_EQ(apple, std::optional<std::string>{"red"});

    auto banana = tree.Get("banana");
    EXPECT_EQ(banana, std::optional<std::string>{"yellow"});

    tree.Put("banana", "green");

    banana = tree.Get("banana");
    EXPECT_EQ(banana, std::optional<std::string>{"green"});
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, RemoveReinsert) {
  const auto path = TestPath("remove");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 3);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    tree.Put("cat", "meow");
    auto cat = tree.Get("cat");
    EXPECT_EQ(cat, std::optional<std::string>{"meow"});

    tree.Remove("cat");
    EXPECT_EQ(tree.Get("cat"), std::nullopt);

    tree.Put("cat", "purr");
    cat = tree.Get("cat");
    EXPECT_EQ(cat, std::optional<std::string>{"purr"});
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, CompactInsert) {
  const auto path = TestPath("compact_insert");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 16);
    const auto root_page_id = NewRootLeaf(&buffer_pool);
    tinydb::BPlusTree tree(&buffer_pool, root_page_id);

    for (int i = 0; i < 38; ++i) {
      tree.Put(TestKey(i), TestValue(i, 80));
    }

    for (int i = 0; i < 30; ++i) {
      tree.Remove(TestKey(i));
    }

    const auto large_value = std::string(1000, 'z');
    tree.Put(TestKey(9999), large_value);

    EXPECT_EQ(tree.Get(TestKey(0)), std::nullopt);
    EXPECT_EQ(tree.Get(TestKey(9999)), std::optional<std::string>{large_value});

    char *root_page = buffer_pool.FetchPage(root_page_id);
    auto *node_header = reinterpret_cast<tinydb::NodeHeader *>(root_page);
    EXPECT_EQ(node_header->type, tinydb::NodeType::Leaf);
    buffer_pool.UnpinPage(root_page_id, false);
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, ScanRange) {
  const auto path = TestPath("scan");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 3);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    tree.Put("mango", "orange");
    tree.Put("apple", "red");
    tree.Put("zebra", "stripe");
    tree.Put("banana", "yellow");
    tree.Remove("mango");

    const auto rows = tree.Scan("apple", "zzzz");
    const auto expected = std::vector<std::pair<std::string, std::string>>{
        {"apple", "red"},
        {"banana", "yellow"},
        {"zebra", "stripe"},
    };

    EXPECT_EQ(rows, expected);
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, RootLeafSplit) {
  const auto path = TestPath("root_split");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 8);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    for (int i = 0; i < 50; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(3) << std::setfill('0') << i;
      const auto key = key_stream.str();
      const auto value = std::string(80, static_cast<char>('a' + (i % 26)));
      tree.Put(key, value);
    }

    for (int i = 0; i < 50; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(3) << std::setfill('0') << i;
      const auto key = key_stream.str();
      const auto value = std::string(80, static_cast<char>('a' + (i % 26)));
      EXPECT_EQ(tree.Get(key), std::optional<std::string>{value});
    }

    const auto rows = tree.Scan("key_010", "key_020");
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().first, "key_010");
    EXPECT_EQ(rows.back().first, "key_019");
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, LeafSplit) {
  const auto path = TestPath("non_root_leaf_split");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 16);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    for (int i = 0; i < 120; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(3) << std::setfill('0') << i;
      const auto key = key_stream.str();
      const auto value = std::string(80, static_cast<char>('a' + (i % 26)));
      tree.Put(key, value);
    }

    for (int i = 0; i < 120; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(3) << std::setfill('0') << i;
      const auto key = key_stream.str();
      const auto value = std::string(80, static_cast<char>('a' + (i % 26)));
      EXPECT_EQ(tree.Get(key), std::optional<std::string>{value});
    }

    const auto rows = tree.Scan("key_000", "key_999");
    ASSERT_EQ(rows.size(), 120);
    EXPECT_EQ(rows.front().first, "key_000");
    EXPECT_EQ(rows.back().first, "key_119");
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, LeftLeafSplit) {
  const auto path = TestPath("left_leaf_split");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 16);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    for (int i = 119; i >= 0; --i) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(3) << std::setfill('0') << i;
      const auto key = key_stream.str();
      const auto value = std::string(80, static_cast<char>('a' + (i % 26)));
      tree.Put(key, value);
    }

    const auto rows = tree.Scan("key_000", "key_999");
    ASSERT_EQ(rows.size(), 120);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(3) << std::setfill('0') << i;
      EXPECT_EQ(rows[i].first, key_stream.str());
    }
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, LeafSplitFit) {
  const auto path = TestPath("large_middle_leaf_record");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 16);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    for (int i = 0; i < 25; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "a_" << std::setw(2) << std::setfill('0') << i;
      tree.Put(key_stream.str(), std::string(90, 'a'));
    }

    for (int i = 0; i < 6; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "z_" << std::setw(2) << std::setfill('0') << i;
      tree.Put(key_stream.str(), std::string(90, 'z'));
    }

    tree.Put("m_large", std::string(1900, 'm'));

    EXPECT_EQ(tree.Get("a_00"),
              std::optional<std::string>{std::string(90, 'a')});
    EXPECT_EQ(tree.Get("m_large"),
              std::optional<std::string>{std::string(1900, 'm')});
    EXPECT_EQ(tree.Get("z_05"),
              std::optional<std::string>{std::string(90, 'z')});
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, InternalSplitFit) {
  const auto path = TestPath("large_middle_internal_separator");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 64);

    auto make_leaf =
        [&buffer_pool](
            const std::vector<std::pair<std::string, std::string>> &rows) {
          tinydb::page_id_t page_id = 0;
          char *page = buffer_pool.NewPage(&page_id);
          auto *header = reinterpret_cast<tinydb::LeafHeader *>(page);
          *header = tinydb::LeafHeader{
              .type = tinydb::NodeType::Leaf,
              .cell_count = 0,
              .free_start = sizeof(tinydb::LeafHeader),
              .free_end = static_cast<std::uint16_t>(tinydb::PAGE_SIZE),
              .next_leaf = tinydb::HEADER_PAGE_ID,
          };
          auto *slots = reinterpret_cast<std::uint16_t *>(
              page + sizeof(tinydb::LeafHeader));

          for (const auto &[key, value] : rows) {
            const auto cell_size =
                sizeof(tinydb::LeafCellHeader) + key.size() + value.size();
            const auto cell_offset = static_cast<std::uint16_t>(
                (header->free_end - cell_size) &
                ~std::size_t{alignof(tinydb::LeafCellHeader) - 1});
            if (header->free_start + sizeof(std::uint16_t) > cell_offset) {
              throw std::runtime_error("test leaf does not fit");
            }

            char *cell = page + cell_offset;
            auto cell_header = tinydb::LeafCellHeader{
                .key_size = static_cast<std::uint16_t>(key.size()),
                .value_size = static_cast<std::uint16_t>(value.size()),
                .flags = 0,
            };
            std::memcpy(cell, &cell_header, sizeof(cell_header));
            std::copy(key.begin(), key.end(),
                      cell + sizeof(tinydb::LeafCellHeader));
            std::copy(value.begin(), value.end(),
                      cell + sizeof(tinydb::LeafCellHeader) + key.size());

            slots[header->cell_count] = cell_offset;
            ++header->cell_count;
            header->free_start = static_cast<std::uint16_t>(
                header->free_start + sizeof(std::uint16_t));
            header->free_end = cell_offset;
          }

          buffer_pool.UnpinPage(page_id, true);
          return page_id;
        };

    const auto long_key = std::string("m_") + std::string(2200, 'x');
    const auto long_value = std::string(100, 'm');

    auto target_rows = std::vector<std::pair<std::string, std::string>>{};
    for (int i = 0; i < 20; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "m_a_" << std::setw(2) << std::setfill('0') << i;
      target_rows.emplace_back(key_stream.str(), std::string(90, 'a'));
    }
    for (int i = 0; i < 5; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "my_" << std::setw(2) << std::setfill('0') << i;
      target_rows.emplace_back(key_stream.str(), std::string(80, 'z'));
    }

    auto leaves = std::vector<tinydb::page_id_t>{};
    leaves.reserve(146);
    for (int i = 0; i < 146; ++i) {
      if (i == 65) {
        leaves.push_back(make_leaf(target_rows));
      } else {
        leaves.push_back(make_leaf({}));
      }
    }

    auto separators = std::vector<std::pair<std::string, tinydb::page_id_t>>{};
    separators.reserve(145);
    for (int i = 0; i < 65; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "a" << std::setw(3) << std::setfill('0') << i;
      separators.emplace_back(key_stream.str(),
                              leaves[static_cast<std::size_t>(i) + 1U]);
    }
    for (int i = 0; i < 80; ++i) {
      auto key_stream = std::ostringstream{};
      key_stream << "z" << std::setw(3) << std::setfill('0') << i;
      separators.emplace_back(key_stream.str(),
                              leaves[66U + static_cast<std::size_t>(i)]);
    }

    tinydb::page_id_t root_page_id = 0;
    char *root_page = buffer_pool.NewPage(&root_page_id);
    auto *root_header = reinterpret_cast<tinydb::InternalHeader *>(root_page);
    *root_header = tinydb::InternalHeader{
        .type = tinydb::NodeType::Internal,
        .cell_count = 0,
        .free_start = sizeof(tinydb::InternalHeader),
        .free_end = static_cast<std::uint16_t>(tinydb::PAGE_SIZE),
        .first_child = leaves.front(),
    };
    auto *root_slots = reinterpret_cast<std::uint16_t *>(
        root_page + sizeof(tinydb::InternalHeader));

    for (const auto &[separator, right_child] : separators) {
      const auto cell_size =
          sizeof(tinydb::InternalCellHeader) + separator.size();
      const auto cell_offset = static_cast<std::uint16_t>(
          (root_header->free_end - cell_size) &
          ~std::size_t{alignof(tinydb::InternalCellHeader) - 1});
      if (root_header->free_start + sizeof(std::uint16_t) > cell_offset) {
        throw std::runtime_error("test internal page does not fit");
      }

      char *cell = root_page + cell_offset;
      auto cell_header = tinydb::InternalCellHeader{
          .right_child = right_child,
          .key_size = static_cast<std::uint16_t>(separator.size()),
      };
      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(separator.begin(), separator.end(),
                cell + sizeof(tinydb::InternalCellHeader));

      root_slots[root_header->cell_count] = cell_offset;
      ++root_header->cell_count;
      root_header->free_start = static_cast<std::uint16_t>(
          root_header->free_start + sizeof(std::uint16_t));
      root_header->free_end = cell_offset;
    }
    buffer_pool.UnpinPage(root_page_id, true);

    tinydb::BPlusTree tree(&buffer_pool, root_page_id);
    tree.Put(long_key, long_value);

    EXPECT_EQ(tree.Get(long_key), std::optional<std::string>{long_value});
    EXPECT_EQ(tree.Get("m_a_00"),
              std::optional<std::string>{std::string(90, 'a')});
    EXPECT_EQ(tree.Get("my_04"),
              std::optional<std::string>{std::string(80, 'z')});
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, InternalSplit) {
  const auto path = TestPath("internal_split");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 32);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

    auto make_key = [](int value) {
      auto key_stream = std::ostringstream{};
      key_stream << "key_" << std::setw(4) << std::setfill('0') << value << "_"
                 << std::string(56, 'x');
      return key_stream.str();
    };

    for (int i = 0; i < 360; ++i) {
      const auto key = make_key(i);
      const auto value = std::string(384, static_cast<char>('a' + (i % 26)));
      tree.Put(key, value);
    }

    for (int i = 0; i < 360; ++i) {
      const auto key = make_key(i);
      const auto value = std::string(384, static_cast<char>('a' + (i % 26)));
      EXPECT_EQ(tree.Get(key), std::optional<std::string>{value});
    }

    const auto rows = tree.Scan(make_key(100), make_key(130));
    ASSERT_EQ(rows.size(), 30);
    EXPECT_EQ(rows.front().first, make_key(100));
    EXPECT_EQ(rows.back().first, make_key(129));
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, RandomOps) {
  const auto path = TestPath("random_ops");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 64);
    tinydb::BPlusTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));
    auto expected = std::map<std::string, std::string>{};
    auto rng = std::mt19937{0xC0FFEEU};
    auto key_dist = std::uniform_int_distribution<int>{0, 319};
    auto value_size_dist = std::uniform_int_distribution<int>{1, 320};

    for (int op = 0; op < 700; ++op) {
      const auto key_id = key_dist(rng);
      const auto value_size = static_cast<std::size_t>(value_size_dist(rng));
      const auto key = TestKey(key_id);
      const auto value = TestValue(op + key_id, value_size);

      tree.Put(key, value);
      expected[key] = value;

      if (op % 50 == 0) {
        CheckTree(&tree, expected);
      }
    }

    CheckTree(&tree, expected);

    auto range_dist = std::uniform_int_distribution<int>{0, 330};
    for (int i = 0; i < 80; ++i) {
      auto start_id = range_dist(rng);
      auto end_id = range_dist(rng);
      if (end_id < start_id) {
        std::swap(start_id, end_id);
      }
      ++end_id;

      const auto start_key = TestKey(start_id);
      const auto end_key = TestKey(end_id);
      auto expected_scan = std::vector<std::pair<std::string, std::string>>{};

      for (auto it = expected.lower_bound(start_key);
           it != expected.end() && it->first < end_key; ++it) {
        expected_scan.emplace_back(it->first, it->second);
      }

      EXPECT_EQ(tree.Scan(start_key, end_key), expected_scan);
    }
  }

  std::filesystem::remove(path);
}

TEST(BPlusTreeTest, Reopen) {
  const auto path = TestPath("reopen_btree");
  std::filesystem::remove(path);

  auto expected = std::map<std::string, std::string>{};
  tinydb::page_id_t root_page_id = tinydb::HEADER_PAGE_ID;

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 32);
    root_page_id = NewRootLeaf(&buffer_pool);
    tinydb::BPlusTree tree(&buffer_pool, root_page_id);

    for (int i = 0; i < 180; ++i) {
      const auto key = TestKey(i);
      const auto value = TestValue(i, 40U + static_cast<std::size_t>(i % 180));
      tree.Put(key, value);
      expected[key] = value;
    }

    CheckTree(&tree, expected);
    buffer_pool.FlushAllPages();
  }

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 32);
    tinydb::BPlusTree tree(&buffer_pool, root_page_id);

    CheckTree(&tree, expected);

    const auto rows = tree.Scan(TestKey(40), TestKey(55));
    auto expected_scan = std::vector<std::pair<std::string, std::string>>{};
    for (auto it = expected.lower_bound(TestKey(40));
         it != expected.end() && it->first < TestKey(55); ++it) {
      expected_scan.emplace_back(it->first, it->second);
    }
    EXPECT_EQ(rows, expected_scan);
  }

  std::filesystem::remove(path);
}
