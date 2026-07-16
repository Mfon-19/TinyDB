#include <gtest/gtest.h>

#include "btree/b_plus_tree.h"

#include "btree/internal_page_builder.h"
#include "btree/leaf_page_builder.h"
#include "btree/page_source.h"
#include "btree/tree_cursor.h"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/*
** This suite runs B+ tree algorithms against the smallest complete PageSource
** model. The harness deliberately has no cache, WAL, or disk behavior. It
** verifies that the tree depends only on lease stability and the four page
** operations, and that allocation failures release every lease without
** leaving a partially reachable tree.
*/
namespace {

// A minimal owner for the logical page-source contract. Heap-owned arrays keep
// page addresses stable when the page table grows, matching the address
// stability that builders and borrowed views require from transaction pages.
class MemoryPageSource final : public tinydb::PageSource {
 public:
  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    const auto page = pages_.find(page_id);
    if (page == pages_.end()) {
      return std::unexpected(tinydb::Status::Corruption("read references an unknown page"));
    }
    ++pins_[page_id];
    return tinydb::PageHandle(this, page_id, page->second->data(), /*editable=*/false, Release);
  }

  auto Edit(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    const auto page = pages_.find(page_id);
    if (page == pages_.end()) {
      return std::unexpected(tinydb::Status::Corruption("edit references an unknown page"));
    }
    ++pins_[page_id];
    return tinydb::PageHandle(this, page_id, page->second->data(), /*editable=*/true, Release);
  }

  auto Allocate() -> tinydb::Result<tinydb::PageHandle> override {
    ++allocation_attempts_;
    if (fail_allocation_.has_value() && allocation_attempts_ == *fail_allocation_) {
      return std::unexpected(tinydb::Status::ResourceExhausted("injected page allocation failure"));
    }

    const auto page_id = next_page_id_++;
    auto [page, inserted] = pages_.emplace(page_id, std::make_unique<Page>());
    EXPECT_TRUE(inserted);
    ++pins_[page_id];
    return tinydb::PageHandle(this, page_id, page->second->data(), /*editable=*/true, Release);
  }

  auto Free(tinydb::page_id_t page_id) -> tinydb::Status override {
    if (pins_[page_id] != 0) {
      return tinydb::Status::Corruption("tree attempted to free a leased page");
    }
    if (pages_.erase(page_id) != 1) {
      return tinydb::Status::Corruption("tree attempted to free an unknown page");
    }
    pins_.erase(page_id);
    return {};
  }

  // Failure numbering starts at the next Allocate. Tests can therefore create
  // the bootstrap root first and enumerate only allocations performed by Put.
  void FailAllocation(std::size_t attempt) {
    allocation_attempts_ = 0;
    fail_allocation_ = attempt;
  }

  void CountAllocations() {
    allocation_attempts_ = 0;
    fail_allocation_.reset();
  }

  auto AllocationAttempts() const -> std::size_t { return allocation_attempts_; }

  auto TotalPins() const -> std::size_t {
    auto result = std::size_t{0};
    for (const auto &[page_id, count] : pins_) {
      static_cast<void>(page_id);
      result += count;
    }
    return result;
  }

 private:
  using Page = std::array<char, tinydb::PAGE_SIZE>;

  static void Release(void *owner, tinydb::page_id_t page_id, bool dirty) {
    // Dirty is a persistence concern; the in-memory owner already exposes the
    // same bytes. Pin balance remains essential and is asserted below.
    static_cast<void>(dirty);
    auto *pages = static_cast<MemoryPageSource *>(owner);
    EXPECT_GT(pages->pins_[page_id], 0U);
    --pages->pins_[page_id];
  }

  tinydb::page_id_t next_page_id_{tinydb::FIRST_DATA_PAGE_ID};
  std::unordered_map<tinydb::page_id_t, std::unique_ptr<Page>> pages_;
  std::unordered_map<tinydb::page_id_t, std::size_t> pins_;
  std::optional<std::size_t> fail_allocation_;
  std::size_t allocation_attempts_{0};
};

auto OpenEmptyTree(MemoryPageSource &pages) -> tinydb::BPlusTree {
  auto root = pages.Allocate().value();
  const auto root_id = root.Id();
  root = tinydb::PageHandle{};
  return tinydb::BPlusTree::Open(&pages, root_id).value();
}

auto PopulateWideKeys(tinydb::BPlusTree &tree) -> tinydb::Status {
  const auto padding = std::string(120, 'p');
  const auto value = std::string(300, 'v');
  for (int row = 0; row < 300; ++row) {
    auto key = std::to_string(row);
    key.insert(0, 6 - key.size(), '0');
    key += padding;
    if (auto status = tree.Put(key, value); !status.Ok()) {
      return status;
    }
  }
  return {};
}

TEST(PageSourceTreeTest, SparseUnderfullPagesRemainCorrect) {
  MemoryPageSource pages;
  auto left_page = pages.Allocate().value();
  auto right_page = pages.Allocate().value();
  auto root_page = pages.Allocate().value();

  auto leaves = tinydb::LeafPageBuilder{};
  leaves.Upsert("alpha", tinydb::LeafValue::Inline("one"));
  leaves.Upsert("omega", tinydb::LeafValue::Inline("two"));
  auto split = leaves.Split(right_page.Id(), /*tail_heavy=*/false);
  leaves.Store(left_page.MutableData(), left_page.Id());
  split.right.Store(right_page.MutableData(), right_page.Id());
  tinydb::InternalPageBuilder(left_page.Id(), std::move(split.separator), right_page.Id())
      .Store(root_page.MutableData(), root_page.Id());
  const auto root_id = root_page.Id();
  left_page = {};
  right_page = {};
  root_page = {};

  auto tree = tinydb::BPlusTree::Open(&pages, root_id).value();
  EXPECT_EQ(tree.Get("alpha").value(), std::optional<std::string>{"one"});
  EXPECT_EQ(tree.Get("omega").value(), std::optional<std::string>{"two"});
  auto cursor = tinydb::BTreeCursor::First(&pages, tree.RootPageId()).value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "alpha");
  EXPECT_EQ(cursor.Value().InlineBytes(), "one");
  ASSERT_TRUE(cursor.Next().Ok());
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "omega");
  EXPECT_EQ(cursor.Value().InlineBytes(), "two");
  ASSERT_TRUE(cursor.Next().Ok());
  EXPECT_FALSE(cursor.Valid());
  EXPECT_EQ(pages.TotalPins(), 0U);
}

TEST(PageSourceTreeTest, InvalidChildReferenceReturnsCorruption) {
  MemoryPageSource pages;
  auto root_page = pages.Allocate().value();
  const auto root_id = root_page.Id();
  tinydb::InternalPageBuilder(/*first_child=*/999, "middle", /*right_child=*/1000)
      .Store(root_page.MutableData(), root_id);
  root_page = {};

  auto tree = tinydb::BPlusTree::Open(&pages, root_id).value();
  const auto value = tree.Get("alpha");
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(pages.TotalPins(), 0U);
}

TEST(PageSourceTreeTest, CursorSeekAndNextBorrowOneLeafAtATime) {
  MemoryPageSource pages;
  auto tree = OpenEmptyTree(pages);
  ASSERT_TRUE(tree.Put("alpha", "one").Ok());
  ASSERT_TRUE(tree.Put("middle", "two").Ok());
  ASSERT_TRUE(tree.Put("omega", "three").Ok());

  auto cursor = tinydb::BTreeCursor::Seek(&pages, tree.RootPageId(), "bravo").value();
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "middle");
  EXPECT_EQ(cursor.Value().InlineBytes(), "two");
  ASSERT_TRUE(cursor.Next().Ok());
  ASSERT_TRUE(cursor.Valid());
  EXPECT_EQ(cursor.Key(), "omega");
  ASSERT_TRUE(cursor.Next().Ok());
  EXPECT_FALSE(cursor.Valid());
}

TEST(PageSourceTreeTest, EverySplitAllocationFailureReturnsResourceExhaustedAndReleasesLeases) {
  MemoryPageSource baseline;
  auto baseline_tree = OpenEmptyTree(baseline);
  baseline.CountAllocations();
  ASSERT_TRUE(PopulateWideKeys(baseline_tree).Ok());
  const auto allocation_count = baseline.AllocationAttempts();
  ASSERT_GT(allocation_count, 3U);  // leaf, internal, and new-root allocations

  for (std::size_t failure = 1; failure <= allocation_count; ++failure) {
    MemoryPageSource pages;
    auto tree = OpenEmptyTree(pages);
    pages.FailAllocation(failure);
    const auto status = PopulateWideKeys(tree);
    ASSERT_EQ(status.Code(), tinydb::StatusCode::ResourceExhausted) << "allocation " << failure;
    EXPECT_EQ(pages.TotalPins(), 0U) << "allocation " << failure;
  }
}

}  // namespace
