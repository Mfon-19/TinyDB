#include <gtest/gtest.h>

#include "cache/page_arena.h"
#include "txn/transaction_pages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

static_assert(sizeof(tinydb::cache::PageBytes) == tinydb::PAGE_SIZE);
static_assert(alignof(tinydb::cache::PageBytes) == alignof(char));

TEST(PageArena, HeapLeasesKeepOrdinaryPageStorage) {
  auto arena = tinydb::cache::PageArena::CreateHeap();
  auto pages = std::array<tinydb::cache::PageArena::Lease, 3>{};
  ASSERT_TRUE(arena->AcquireBatch(pages));

  for (auto index = std::size_t{0}; index < pages.size(); ++index) {
    ASSERT_TRUE(static_cast<bool>(pages[index]));
    pages[index]->front() = static_cast<char>(index + 1U);
  }

  auto moved = std::move(pages[1]);
  EXPECT_FALSE(static_cast<bool>(pages[1]));
  ASSERT_TRUE(static_cast<bool>(moved));
  EXPECT_EQ(static_cast<unsigned char>(moved->front()), 2U);
}

TEST(PageArena, DirectBatchIsAlignedAndContiguous) {
  auto arena = tinydb::cache::PageArena::CreateDirect(8);
  auto pages = std::array<tinydb::cache::PageArena::Lease, 4>{};
  ASSERT_TRUE(arena->AcquireBatch(pages));

  const auto first = reinterpret_cast<std::uintptr_t>(pages.front()->data());
  ASSERT_EQ(first % tinydb::PAGE_SIZE, 0U);
  for (auto index = std::size_t{0}; index < pages.size(); ++index) {
    const auto address = reinterpret_cast<std::uintptr_t>(pages[index]->data());
    EXPECT_EQ(address, first + index * tinydb::PAGE_SIZE);
    pages[index]->front() = static_cast<char>(index + 1U);
  }
}

TEST(PageArena, DirectReleaseCoalescesAReusableRun) {
  auto arena = tinydb::cache::PageArena::CreateDirect(4);
  auto pages = std::array<tinydb::cache::PageArena::Lease, 4>{};
  ASSERT_TRUE(arena->AcquireBatch(pages));
  auto *const expected = pages[1]->data();

  pages[1] = {};
  pages[2] = {};

  auto reused = std::array<tinydb::cache::PageArena::Lease, 2>{};
  ASSERT_TRUE(arena->AcquireBatch(reused));
  EXPECT_EQ(reused[0]->data(), expected);
  EXPECT_EQ(reused[1]->data(), expected + tinydb::PAGE_SIZE);
}

TEST(PageArena, SmallDirectTargetStillUsesOneLazySlabForManyLeases) {
  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  auto pages = std::array<tinydb::cache::PageArena::Lease, 64>{};
  for (auto &page : pages) {
    page = arena->Acquire();
    ASSERT_TRUE(page);
  }

  const auto first = reinterpret_cast<std::uintptr_t>(pages.front()->data());
  for (auto index = std::size_t{0}; index < pages.size(); ++index) {
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pages[index]->data()), first + index * tinydb::PAGE_SIZE);
  }
}

TEST(PageArena, AbortedTransactionReleasesFreeDirectMemoryInOneBatch) {
  auto arena = tinydb::cache::PageArena::CreateDirect(1);
  const char *address = nullptr;
  {
    auto transaction = tinydb::txn::TransactionPages::Begin(
        nullptr, tinydb::txn::DatabaseState{.logical_page_count = tinydb::FIRST_DATA_PAGE_ID}, tinydb::PAGE_SIZE,
        arena);
    ASSERT_TRUE(transaction.has_value()) << transaction.error().ToString();
    auto page = transaction->Allocate();
    ASSERT_TRUE(page.has_value()) << page.error().ToString();
    address = page->MutableData();
    std::ranges::fill(std::span<char, tinydb::PAGE_SIZE>{page->MutableData(), tinydb::PAGE_SIZE}, 'x');
  }

  auto reused = arena->Acquire();
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->data(), address);
  EXPECT_TRUE(std::ranges::all_of(*reused, [](char byte) { return byte == 0; }));
}

}  // namespace
