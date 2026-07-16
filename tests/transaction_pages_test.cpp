#include <gtest/gtest.h>

#include "btree/b_plus_tree.h"
#include "btree/leaf_page_builder.h"
#include "btree/page_source.h"
#include "storage/page_codec.h"
#include "txn/database_state.h"
#include "txn/transaction_pages.h"
#include "verify/verifier.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

/*
** Transaction overlay tests compare private allocation and mutation with a
** simple immutable committed-page model. Tests cover definite abort at every
** memory boundary, checkpoint-gated reuse, allocator-index growth, root
** changes, and randomized commit/abort sequences. Publishing in this harness
** is explicit so assertions can inspect base state before and after transfer.
*/
namespace {

class MemoryCommitted final : public tinydb::PageReader {
 public:
  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    const auto page = pages_.find(page_id);
    if (page == pages_.end()) {
      return std::unexpected(tinydb::Status::Corruption("missing committed page"));
    }
    return tinydb::PageHandle(page->second.get(), page_id, page->second->data(), nullptr,
                              std::static_pointer_cast<const void>(page->second));
  }

  void Put(tinydb::page_id_t page_id, tinydb::cache::PageBytes page) {
    pages_[page_id] = std::make_shared<tinydb::cache::PageBytes>(std::move(page));
  }

  void Publish(std::vector<tinydb::cache::CommittedPageImage> pages, std::span<const tinydb::page_id_t> retired) {
    for (auto &page : pages) {
      pages_[page.page_id] = std::shared_ptr<tinydb::cache::PageBytes>(std::move(page.bytes));
    }
    for (const auto page_id : retired) {
      pages_.erase(page_id);
    }
  }

 private:
  std::map<tinydb::page_id_t, std::shared_ptr<tinydb::cache::PageBytes>> pages_;
};

auto Leaf(tinydb::page_id_t page_id,
          std::initializer_list<std::pair<std::string_view, std::string_view>> rows) -> tinydb::cache::PageBytes {
  auto builder = tinydb::LeafPageBuilder{};
  for (const auto &[key, value] : rows) {
    builder.Upsert(key, tinydb::LeafValue::Inline(value));
  }
  auto page = tinydb::cache::PageBytes{};
  builder.Store(page.data(), page_id);
  return page;
}

auto BaseState(tinydb::page_id_t high_water = 3) -> tinydb::txn::DatabaseState {
  return tinydb::txn::DatabaseState{
      .root_page_id = 2,
      .allocator_root_page_id = tinydb::HEADER_PAGE_ID,
      .high_water_page_id = high_water,
      .transaction_id = 1,
      .visible_lsn = 1,
      .checkpoint_lsn = 1,
  };
}

auto OpenTree(tinydb::txn::TransactionPages &pages, tinydb::page_id_t root_page_id) -> tinydb::BPlusTree {
  return tinydb::BPlusTree::Open(&pages, root_page_id).value();
}

TEST(TransactionPagesTest, EditAndAbortNeverChangeCommittedBytes) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {{"key", "old"}}));
  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, BaseState(), 4 * tinydb::PAGE_SIZE).value();
  auto tree = OpenTree(transaction, 2);
  ASSERT_TRUE(tree.Put("key", "private").Ok());
  EXPECT_EQ(tree.Get("key").value(), std::optional<std::string>{"private"});

  auto outside = tinydb::LeafPageView::Open(committed.Read(2)->Data(), 2).value();
  ASSERT_TRUE(outside.Get("key").has_value());
  EXPECT_EQ(outside.Get("key")->InlineBytes(), "old");

  transaction.Abort();
  auto after = tinydb::LeafPageView::Open(committed.Read(2)->Data(), 2).value();
  ASSERT_TRUE(after.Get("key").has_value());
  EXPECT_EQ(after.Get("key")->InlineBytes(), "old");
}

TEST(TransactionPagesTest, MemoryLimitFailsBeforeASecondPrivatePageAppears) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {{"a", "one"}}));
  committed.Put(3, Leaf(3, {{"b", "two"}}));
  auto state = BaseState(4);
  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, tinydb::PAGE_SIZE).value();

  auto first = transaction.Edit(2);
  ASSERT_TRUE(first.has_value());
  first = tinydb::PageHandle{};
  const auto second = transaction.Edit(3);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().Code(), tinydb::StatusCode::ResourceExhausted);
  EXPECT_EQ(transaction.PrivatePageCount(), 1U);
  EXPECT_EQ(transaction.MemoryUsedBytes(), tinydb::PAGE_SIZE);
}

TEST(TransactionPagesTest, ValueBuffersAndPagesShareOneMemoryBudget) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {{"a", "one"}}));
  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, BaseState(), tinydb::PAGE_SIZE + 16).value();
  ASSERT_TRUE(transaction.ChargeValueBytes(16).Ok());
  EXPECT_EQ(transaction.MemoryUsedBytes(), 16U);
  ASSERT_TRUE(transaction.Edit(2).has_value());
  EXPECT_EQ(transaction.MemoryUsedBytes(), tinydb::PAGE_SIZE + 16);
  EXPECT_EQ(transaction.ChargeValueBytes(1).Code(), tinydb::StatusCode::ResourceExhausted);
}

TEST(TransactionPagesTest, AbortedHighWaterAllocationIsReusedByTheNextTransaction) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  const auto state = BaseState();
  {
    auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 2 * tinydb::PAGE_SIZE).value();
    auto page = transaction.Allocate().value();
    EXPECT_EQ(page.Id(), 3U);
    page = {};
    transaction.Abort();
  }
  auto retry = tinydb::txn::TransactionPages::Begin(&committed, state, 2 * tinydb::PAGE_SIZE).value();
  EXPECT_EQ(retry.Allocate()->Id(), 3U);
}

TEST(TransactionPagesTest, ReuseWaitsForCheckpointAndNeverUsesSameTransactionRetirements) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  const auto extents = std::array{
      tinydb::storage::FreeExtent{.first_page_id = 3, .page_count = 1, .retire_lsn = 4},
      tinydb::storage::FreeExtent{.first_page_id = 5, .page_count = 1, .retire_lsn = 9},
  };
  committed.Put(6, *tinydb::storage::EncodeFreeExtentPage(6, 5, 0, extents));
  auto state = BaseState(7);
  state.allocator_root_page_id = 6;
  state.checkpoint_lsn = 5;

  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 8 * tinydb::PAGE_SIZE).value();
  auto reused = transaction.Allocate().value();
  EXPECT_EQ(reused.Id(), 3U);
  reused = {};
  ASSERT_TRUE(transaction.Free(3).Ok());
  auto next = transaction.Allocate().value();
  EXPECT_EQ(next.Id(), 7U);  // extent 5 is too new; retired 3 cannot cycle back.
}

TEST(TransactionPagesTest, FreeIndexGrowthUsesOnlyTheHighWaterFrontier) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  auto extents = std::vector<tinydb::storage::FreeExtent>{};
  for (std::uint64_t index = 0; index < tinydb::storage::FREE_EXTENTS_PER_PAGE; ++index) {
    extents.push_back(tinydb::storage::FreeExtent{
        .first_page_id = 4 + index * 2,
        .page_count = 1,
        .retire_lsn = 100,
    });
  }
  committed.Put(3, *tinydb::storage::EncodeFreeExtentPage(3, 1, 0, extents));
  auto state = BaseState(500);
  state.allocator_root_page_id = 3;
  state.checkpoint_lsn = 1;
  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 8 * tinydb::PAGE_SIZE).value();

  ASSERT_TRUE(transaction.Free(400).Ok());
  ASSERT_TRUE(transaction.Freeze().Ok());
  ASSERT_TRUE(transaction.Seal(2).Ok());
  EXPECT_EQ(transaction.ResultingState().high_water_page_id, 501U);
  EXPECT_EQ(transaction.AllocatorPageIds(), (std::vector<tinydb::page_id_t>{3, 500}));
  const auto images = transaction.PageImages();
  EXPECT_TRUE(std::ranges::any_of(images, [](const auto &image) { return image.first == 500; }));
}

TEST(TransactionPagesTest, AllocationAndAbortMatchAReferenceFrontierModel) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  auto state = BaseState();
  auto random = std::mt19937_64{0x51A7EULL};
  auto live = std::unordered_set<tinydb::page_id_t>{2};

  for (std::size_t round = 0; round < 100; ++round) {
    if (round % 7 == 0) {
      state.checkpoint_lsn = state.visible_lsn;
    }
    auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 64 * tinydb::PAGE_SIZE).value();
    auto allocated = std::unordered_set<tinydb::page_id_t>{};
    const auto allocations = 1U + static_cast<unsigned>(random() % 20U);
    for (unsigned index = 0; index < allocations; ++index) {
      auto page = transaction.Allocate().value();
      EXPECT_TRUE(allocated.insert(page.Id()).second);
      EXPECT_FALSE(live.contains(page.Id()));
      tinydb::LeafPageBuilder{}.Store(page.MutableData(), page.Id());
      page.MarkDirty();
    }
    auto retired = std::vector<tinydb::page_id_t>{};
    for (const auto page_id : live) {
      if (page_id != state.root_page_id && (random() % 4U) == 0U) {
        retired.push_back(page_id);
      }
    }
    for (const auto page_id : retired) {
      ASSERT_TRUE(transaction.Free(page_id).Ok());
    }
    if ((random() & 1U) == 0U) {
      transaction.Abort();
      continue;
    }
    ASSERT_TRUE(transaction.Freeze().Ok());
    ASSERT_TRUE(transaction.Seal(state.visible_lsn + 1).Ok());
    const auto next_state = transaction.ResultingState();
    const auto retired_ids =
        std::vector<tinydb::page_id_t>(transaction.RetiredPageIds().begin(), transaction.RetiredPageIds().end());
    auto pages = transaction.TakePages(next_state.transaction_id).value();
    committed.Publish(std::move(pages), retired_ids);
    live.insert(allocated.begin(), allocated.end());
    for (const auto page_id : retired_ids) {
      live.erase(page_id);
    }
    state = next_state;
  }
}

TEST(TransactionPagesTest, BTreeAllocationFailureAbortsWithoutChangingCommittedTree) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  const auto state = BaseState();
  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 2 * tinydb::PAGE_SIZE).value();
  auto tree = OpenTree(transaction, 2);

  auto failed = false;
  for (std::size_t index = 0; index < 100; ++index) {
    const auto status = tree.Put(std::to_string(index), std::string(900, 'v'));
    if (!status.Ok()) {
      EXPECT_EQ(status.Code(), tinydb::StatusCode::ResourceExhausted);
      failed = true;
      break;
    }
  }
  ASSERT_TRUE(failed);
  transaction.Abort();

  auto retry = tinydb::txn::TransactionPages::Begin(&committed, state, 8 * tinydb::PAGE_SIZE).value();
  auto clean_tree = OpenTree(retry, 2);
  EXPECT_EQ(clean_tree.Get("0").value(), std::nullopt);
  EXPECT_TRUE(clean_tree.Put("healthy", "yes").Ok());
}

TEST(TransactionPagesTest, EveryBTreePageBudgetFailureLeavesTheBaseTreeReusable) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  const auto state = BaseState();

  auto successful_page_count = std::size_t{0};
  {
    auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 128 * tinydb::PAGE_SIZE).value();
    auto tree = OpenTree(transaction, 2);
    for (std::size_t index = 0; index < 80; ++index) {
      ASSERT_TRUE(tree.Put(std::to_string(index), std::string(700, 'v')).Ok());
    }
    successful_page_count = transaction.PrivatePageCount();
  }
  ASSERT_GT(successful_page_count, 3U);

  for (std::size_t page_budget = 1; page_budget < successful_page_count; ++page_budget) {
    auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, page_budget * tinydb::PAGE_SIZE).value();
    auto tree = OpenTree(transaction, 2);
    auto failed = false;
    for (std::size_t index = 0; index < 80; ++index) {
      const auto status = tree.Put(std::to_string(index), std::string(700, 'v'));
      if (!status.Ok()) {
        EXPECT_EQ(status.Code(), tinydb::StatusCode::ResourceExhausted);
        failed = true;
        break;
      }
    }
    EXPECT_TRUE(failed) << "page_budget=" << page_budget;
    transaction.Abort();

    auto probe = tinydb::txn::TransactionPages::Begin(&committed, state, 2 * tinydb::PAGE_SIZE).value();
    EXPECT_EQ(OpenTree(probe, 2).Get("0").value(), std::nullopt);
  }
}

TEST(TransactionPagesTest, RootChangesAllocateOneUniqueReachablePageSet) {
  auto committed = MemoryCommitted{};
  committed.Put(2, Leaf(2, {}));
  const auto state = BaseState();
  auto transaction = tinydb::txn::TransactionPages::Begin(&committed, state, 256 * tinydb::PAGE_SIZE).value();
  auto tree = OpenTree(transaction, 2);
  for (std::size_t index = 0; index < 500; ++index) {
    ASSERT_TRUE(tree.Put("key-" + std::to_string(index), std::string(300, 'v')).Ok());
  }
  ASSERT_NE(tree.RootPageId(), state.root_page_id);
  transaction.SetRootPageId(tree.RootPageId());
  ASSERT_TRUE(transaction.Freeze().Ok());
  ASSERT_TRUE(transaction.Seal(2).Ok());
  const auto verified = tinydb::verify::Snapshot(&transaction, transaction.ResultingState(), 256 * tinydb::PAGE_SIZE);
  ASSERT_TRUE(verified.has_value()) << verified.error().ToString();
  EXPECT_TRUE(verified->report.Ok());
}

}  // namespace
