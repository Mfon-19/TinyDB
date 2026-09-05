#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/cache/buffer_pool.h"
#include "tinydb/storage/disk_manager.h"
#include "tinydb/storage/page.h"
#include "tinydb/storage/page_codec.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace tinydb::btree {

namespace {

auto TempFile() -> std::string {
  std::string path = testing::TempDir() + "b_plus_tree_test_XXXXXX";
  close(mkstemp(path.data()));
  return path;
}

struct TestTree {
  std::string path = TempFile();
  cache::BufferPool pool{storage::DiskManager::Open(path).value(), 8};
  std::atomic<bool> poisoned{false};
  detail::WriteState state{2, {}};
  detail::PageContext context{pool, poisoned, &state};
  storage::PageId root = 1;
  BPlusTree tree{context, root};

  TestTree() {
    const Status status = tree.Initialize();
    if (!status.Ok()) {
      throw std::runtime_error(std::string{status.Message()});
    }
  }
  ~TestTree() { unlink(path.c_str()); }
};

auto IsOk(const Status &status) -> testing::AssertionResult {
  if (status.Ok()) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << status.Message();
}

auto Lookup(BPlusTree &tree,
            std::string_view key) -> std::optional<std::string> {
  auto result = tree.Get(key);
  if (!result) {
    ADD_FAILURE() << "Get(" << key << "): " << result.error().Message();
  }
  return result.value_or(std::nullopt);
}

auto Remove(BPlusTree &tree, std::string_view key) -> bool {
  auto result = tree.Delete(key);
  if (!result) {
    ADD_FAILURE() << "Delete(" << key << "): " << result.error().Message();
  }
  return result.value_or(false);
}

auto LeftmostPath(detail::PageContext &context,
                  storage::PageId page_id) -> std::vector<storage::PageId> {
  std::vector<storage::PageId> path{page_id};
  while (true) {
    const auto page = context.ReadPage(page_id).value();
    if (page.Type() != storage::PageType::Internal) {
      return path;
    }
    page_id = page.Internal().LeftmostChild();
    path.push_back(page_id);
  }
}

auto LeafKeys(detail::PageContext &context,
              storage::PageId root) -> std::vector<std::string> {
  std::vector<std::string> keys;
  storage::PageId page_id = LeftmostPath(context, root).back();
  while (page_id != storage::INVALID_PAGE_ID) {
    const auto page = context.ReadPage(page_id).value();
    const auto leaf = page.Leaf();
    for (std::size_t index = 0; index < leaf.EntryCount(); ++index) {
      keys.emplace_back(leaf.Entry(index).key);
    }
    page_id = leaf.NextLeaf();
  }
  return keys;
}

void CollectLeafDepths(detail::PageContext &context, storage::PageId page_id,
                       std::size_t depth, std::vector<std::size_t> &depths) {
  const auto page = context.ReadPage(page_id).value();
  if (page.Type() != storage::PageType::Internal) {
    depths.push_back(depth);
    return;
  }

  const auto internal = page.Internal();
  CollectLeafDepths(context, internal.LeftmostChild(), depth + 1, depths);
  for (std::size_t index = 0; index < internal.EntryCount(); ++index) {
    CollectLeafDepths(context, internal.Entry(index).right_child, depth + 1,
                      depths);
  }
}

auto LeafDepths(detail::PageContext &context,
                storage::PageId root) -> std::vector<std::size_t> {
  std::vector<std::size_t> depths;
  CollectLeafDepths(context, root, 0, depths);
  return depths;
}

auto Key(std::size_t index) -> std::string {
  return std::format("k{:06}", index);
}

auto Value(std::string_view key, std::size_t size) -> std::string {
  std::string value;
  while (value.size() < size) {
    value += key;
    value += '/';
  }
  value.resize(size);
  return value;
}

void Fill(BPlusTree &tree, std::size_t count, std::size_t value_size) {
  for (std::size_t i = 0; i < count; ++i) {
    EXPECT_TRUE(IsOk(tree.Put(Key(i), Value(Key(i), value_size)))) << Key(i);
  }
}

void ExpectFilled(BPlusTree &tree, std::size_t count, std::size_t value_size) {
  for (std::size_t i = 0; i < count; ++i) {
    EXPECT_EQ(Lookup(tree, Key(i)), Value(Key(i), value_size)) << Key(i);
  }
}

TEST(BPlusTree, GetReturnsWhatPutStored) {
  TestTree t;
  EXPECT_EQ(Lookup(t.tree, "key"), std::nullopt);
  ASSERT_TRUE(IsOk(t.tree.Put("key", "value")));
  EXPECT_EQ(Lookup(t.tree, "key"), "value");
  EXPECT_EQ(Lookup(t.tree, "ke"), std::nullopt);
  EXPECT_EQ(Lookup(t.tree, "key2"), std::nullopt);
}

TEST(BPlusTree, PutOverwritesAnExistingValue) {
  TestTree t;
  ASSERT_TRUE(IsOk(t.tree.Put("key", "short")));
  ASSERT_TRUE(IsOk(t.tree.Put("key", "a much longer value than before")));
  EXPECT_EQ(Lookup(t.tree, "key"), "a much longer value than before");
  ASSERT_TRUE(IsOk(t.tree.Put("key", "")));
  EXPECT_EQ(Lookup(t.tree, "key"), "");
}

TEST(BPlusTree, DeleteRemovesOnlyItsKey) {
  TestTree t;
  EXPECT_FALSE(Remove(t.tree, "key"));
  ASSERT_TRUE(IsOk(t.tree.Put("key", "value")));
  ASSERT_TRUE(IsOk(t.tree.Put("other", "kept")));
  EXPECT_TRUE(Remove(t.tree, "key"));
  EXPECT_FALSE(Remove(t.tree, "key"));
  EXPECT_EQ(Lookup(t.tree, "key"), std::nullopt);
  EXPECT_EQ(Lookup(t.tree, "other"), "kept");
}

TEST(BPlusTree, CapsAnEntryAtAQuarterPage) {
  TestTree t;
  const std::string key(24, 'k');
  const std::string largest(MAX_ENTRY_SIZE - key.size(), 'v');
  ASSERT_TRUE(IsOk(t.tree.Put(key, largest)));
  EXPECT_EQ(Lookup(t.tree, key), largest);
  EXPECT_FALSE(t.tree.Put(key, largest + "v").Ok());
  EXPECT_EQ(Lookup(t.tree, key), largest);
}

TEST(BPlusTree, GrowsAcrossManySplits) {
  TestTree t;
  Fill(t.tree, 3000, 600);
  ExpectFilled(t.tree, 3000, 600);
  EXPECT_EQ(Lookup(t.tree, Key(3000)), std::nullopt);
}

TEST(BPlusTree, MatchesAMapUnderRandomOperations) {
  TestTree t;
  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> key_size(1, 32);
  std::uniform_int_distribution<std::size_t> value_size(0, 200);
  std::uniform_int_distribution<int> letter('a', 'z');
  auto random_string = [&](std::size_t size) {
    std::string text(size, '\0');
    for (auto &c : text) {
      c = static_cast<char>(letter(rng));
    }
    return text;
  };

  std::map<std::string, std::string> model;
  for (int i = 0; i < 4000; ++i) {
    const std::string key = random_string(key_size(rng));
    model[key] = random_string(value_size(rng));
    ASSERT_TRUE(IsOk(t.tree.Put(key, model[key])));
  }

  std::vector<std::string> removed;
  std::size_t index = 0;
  for (auto it = model.begin(); it != model.end(); ++index) {
    if (index % 3 == 0) {
      EXPECT_TRUE(Remove(t.tree, it->first));
      removed.push_back(it->first);
      it = model.erase(it);
      continue;
    }
    if (index % 5 == 0) {
      it->second = random_string(value_size(rng));
      ASSERT_TRUE(IsOk(t.tree.Put(it->first, it->second)));
    }
    ++it;
  }
  for (const auto &[key, value] : model) {
    EXPECT_EQ(Lookup(t.tree, key), value);
  }
  for (const auto &key : removed) {
    EXPECT_EQ(Lookup(t.tree, key), std::nullopt);
  }

  std::vector<std::string> expected;
  for (const auto &[key, value] : model) {
    expected.push_back(key);
  }
  EXPECT_EQ(LeafKeys(t.context, t.root), expected);
  EXPECT_TRUE(t.tree.FindFreePages(t.state.page_count));
  const auto depths = LeafDepths(t.context, t.root);
  EXPECT_TRUE(std::ranges::all_of(
      depths, [&](std::size_t depth) { return depth == depths.front(); }));
}

TEST(BPlusTree, DeleteEverythingThenReinsert) {
  TestTree t;
  Fill(t.tree, 1000, 1000);
  ASSERT_GE(LeftmostPath(t.context, t.root).size(), 3U);
  const storage::PageId page_count = t.state.page_count;
  for (std::size_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(Remove(t.tree, Key(i))) << Key(i);
    EXPECT_EQ(Lookup(t.tree, Key(i)), std::nullopt) << Key(i);
  }
  EXPECT_EQ(LeftmostPath(t.context, t.root).size(), 1U);
  EXPECT_TRUE(LeafKeys(t.context, t.root).empty());

  auto free_pages = t.tree.FindFreePages(t.state.page_count);
  ASSERT_TRUE(free_pages);
  t.state.free_pages = std::move(*free_pages);
  Fill(t.tree, 1000, 250);
  ExpectFilled(t.tree, 1000, 250);
  EXPECT_EQ(t.state.page_count, page_count);
}

TEST(BPlusTree, PersistsAcrossReopen) {
  const std::string path = TempFile();
  constexpr storage::PageId root = 1;
  const std::atomic<bool> poisoned{false};
  {
    cache::BufferPool pool{storage::DiskManager::Open(path).value(), 8};
    detail::WriteState state{2, {}};
    detail::PageContext context{pool, poisoned, &state};
    BPlusTree tree{context, root};
    ASSERT_TRUE(IsOk(tree.Initialize()));
    Fill(tree, 800, 400);
    EXPECT_TRUE(Remove(tree, Key(7)));
    ASSERT_TRUE(IsOk(pool.Checkpoint(state.pages)));
  }

  cache::BufferPool pool{storage::DiskManager::Open(path).value(), 8};
  detail::PageContext context{pool, poisoned};
  BPlusTree tree{context, root};
  ExpectFilled(tree, 7, 400);
  EXPECT_EQ(Lookup(tree, Key(7)), std::nullopt);
  for (std::size_t i = 8; i < 800; ++i) {
    EXPECT_EQ(Lookup(tree, Key(i)), Value(Key(i), 400)) << Key(i);
  }
  unlink(path.c_str());
}

} // namespace

} // namespace tinydb::btree
