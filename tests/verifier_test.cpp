#include <gtest/gtest.h>

#include "btree/internal_page_builder.h"
#include "btree/leaf_page_builder.h"
#include "btree/page_format.h"
#include "btree/page_source.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "txn/database_state.h"
#include "verify/verifier.h"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/*
** VERIFIER HOSTILE-PAGE MODEL
**
** These tests bypass Database::Open, which correctly rejects corrupt state
** before returning a handle.  The in-memory reader supplies individually
** checksummed pages so each test can violate one cross-page invariant while
** every local page decoder still accepts the remaining bytes.
*/
namespace {

class MemoryPages final : public tinydb::PageReader {
 public:
  void Put(tinydb::page_id_t page_id, std::array<char, tinydb::PAGE_SIZE> bytes) {
    pages_[page_id] = std::make_shared<std::array<char, tinydb::PAGE_SIZE>>(std::move(bytes));
  }

  auto Mutable(tinydb::page_id_t page_id) -> std::array<char, tinydb::PAGE_SIZE> & { return *pages_.at(page_id); }

  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    const auto found = pages_.find(page_id);
    if (found == pages_.end()) {
      return std::unexpected(tinydb::Status::Corruption("test page is absent"));
    }
    return tinydb::PageHandle(found->second.get(), page_id, found->second->data(), nullptr,
                              std::static_pointer_cast<const void>(found->second));
  }

 private:
  std::unordered_map<tinydb::page_id_t, std::shared_ptr<std::array<char, tinydb::PAGE_SIZE>>> pages_;
};

auto Leaf(tinydb::page_id_t page_id, std::string key, tinydb::page_id_t next = tinydb::HEADER_PAGE_ID)
    -> std::array<char, tinydb::PAGE_SIZE> {
  auto builder = tinydb::LeafPageBuilder{};
  EXPECT_TRUE(builder.Upsert(key, tinydb::LeafValue::Inline("value")));
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  builder.Store(page.data(), page_id);
  if (next != tinydb::HEADER_PAGE_ID) {
    auto bytes = std::as_writable_bytes(std::span{page});
    EXPECT_TRUE(tinydb::storage::PutLittleEndian(bytes, tinydb::node_page_offset::LINK, next));
    EXPECT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  }
  return page;
}

auto Internal(tinydb::page_id_t page_id, tinydb::page_id_t left, std::string separator,
              tinydb::page_id_t right) -> std::array<char, tinydb::PAGE_SIZE> {
  auto builder = tinydb::InternalPageBuilder(left, std::move(separator), right);
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  builder.Store(page.data(), page_id);
  return page;
}

auto State(tinydb::page_id_t high_water = 5, tinydb::page_id_t allocator_root = tinydb::HEADER_PAGE_ID)
    -> tinydb::txn::DatabaseState {
  return tinydb::txn::DatabaseState{
      .root_page_id = 2,
      .allocator_root_page_id = allocator_root,
      .high_water_page_id = high_water,
      .transaction_id = 7,
      .visible_lsn = 11,
      .checkpoint_lsn = 9,
  };
}

auto Tree() -> MemoryPages {
  auto pages = MemoryPages{};
  pages.Put(2, Internal(2, 3, "m", 4));
  pages.Put(3, Leaf(3, "a", 4));
  pages.Put(4, Leaf(4, "z"));
  return pages;
}

auto Contains(const tinydb::VerifyReport &report, tinydb::VerifyIssueKind kind) -> bool {
  for (const auto &issue : report.issues) {
    if (issue.kind == kind) {
      return true;
    }
  }
  return false;
}

auto Verify(MemoryPages *pages, tinydb::txn::DatabaseState state) -> tinydb::VerifyReport {
  const auto result = tinydb::verify::Snapshot(pages, state, 64U * tinydb::PAGE_SIZE);
  EXPECT_TRUE(result.has_value()) << result.error().ToString();
  return result ? result->report : tinydb::VerifyReport{};
}

TEST(VerifierTest, ValidSnapshotAccountsForTreeAllocatorAndRetirementDomains) {
  auto pages = Tree();
  const auto reusable = std::array{
      tinydb::storage::FreeExtent{.first_page_id = 6, .page_count = 1, .retire_lsn = 8},
  };
  const auto retired = std::array{
      tinydb::storage::FreeExtent{.first_page_id = 8, .page_count = 1, .retire_lsn = 10},
  };
  pages.Put(5, tinydb::storage::EncodeFreeExtentPage(5, 9, 7, reusable).value());
  pages.Put(7, tinydb::storage::EncodeFreeExtentPage(7, 9, tinydb::HEADER_PAGE_ID, retired).value());

  const auto report = Verify(&pages, State(9, 5));
  EXPECT_TRUE(report.Ok());
  EXPECT_EQ(report.internal_pages, 1U);
  EXPECT_EQ(report.leaf_pages, 2U);
  EXPECT_EQ(report.allocator_pages, 2U);
  EXPECT_EQ(report.reusable_pages, 1U);
  EXPECT_EQ(report.retired_pages, 1U);
  EXPECT_EQ(report.pages_checked, 5U);
}

TEST(VerifierTest, ReportsChecksumsRoutingAndLeafLinksByClass) {
  {
    auto pages = Tree();
    pages.Mutable(3).back() ^= 0x01;
    const auto report = Verify(&pages, State());
    EXPECT_FALSE(report.Ok());
    EXPECT_TRUE(Contains(report, tinydb::VerifyIssueKind::Page));
  }
  {
    auto pages = Tree();
    pages.Put(4, Leaf(4, "b"));
    const auto report = Verify(&pages, State());
    EXPECT_FALSE(report.Ok());
    EXPECT_TRUE(Contains(report, tinydb::VerifyIssueKind::TreeStructure));
  }
  {
    auto pages = Tree();
    pages.Put(3, Leaf(3, "a"));
    const auto report = Verify(&pages, State());
    EXPECT_FALSE(report.Ok());
    EXPECT_TRUE(Contains(report, tinydb::VerifyIssueKind::LeafLink));
  }
}

TEST(VerifierTest, ReportsDoubleAllocationAndLeakedPagesSeparately) {
  {
    auto pages = Tree();
    const auto extents = std::array{
        tinydb::storage::FreeExtent{.first_page_id = 3, .page_count = 1, .retire_lsn = 8},
    };
    pages.Put(5, tinydb::storage::EncodeFreeExtentPage(5, 9, tinydb::HEADER_PAGE_ID, extents).value());
    const auto report = Verify(&pages, State(6, 5));
    EXPECT_FALSE(report.Ok());
    EXPECT_TRUE(Contains(report, tinydb::VerifyIssueKind::DoubleAllocation));
  }
  {
    auto pages = Tree();
    const auto report = Verify(&pages, State(6));
    EXPECT_FALSE(report.Ok());
    EXPECT_TRUE(Contains(report, tinydb::VerifyIssueKind::LeakedPage));
  }
}

}  // namespace
