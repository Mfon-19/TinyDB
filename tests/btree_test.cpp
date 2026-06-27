#include <gtest/gtest.h>
#include <tinydb/btree.h>

#include <unistd.h>

#include <filesystem>
#include <optional>
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
      .type = 1,
      .cell_count = 0,
      .free_start = sizeof(tinydb::LeafHeader),
      .free_end = tinydb::PAGE_SIZE,
  };

  buffer_pool->UnpinPage(root_page_id, true);
  return root_page_id;
}

TEST(BTreeTest, PutGetAndOverwrite) {
  const auto path = TestPath("put_get");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 3);
    tinydb::BTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

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

TEST(BTreeTest, RemoveHidesValueAndAllowsReinsert) {
  const auto path = TestPath("remove");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 3);
    tinydb::BTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

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

TEST(BTreeTest, ScanReturnsSortedRangeAndSkipsTombstones) {
  const auto path = TestPath("scan");
  std::filesystem::remove(path);

  {
    tinydb::DiskManager disk(path);
    tinydb::BufferPool buffer_pool(&disk, 3);
    tinydb::BTree tree(&buffer_pool, NewRootLeaf(&buffer_pool));

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
