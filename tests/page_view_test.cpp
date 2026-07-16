#include <gtest/gtest.h>

#include "btree/internal_node.h"
#include "btree/leaf_node.h"
#include "btree/page_format.h"
#include "btree/page_view.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

TEST(LeafPageViewTest, SearchesEncodedRecordsWithoutOwningThem) {
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::LeafNode{};
  builder.Upsert("alpha", "one");
  builder.Upsert("middle", "two");
  builder.Upsert("omega", "three");
  builder.Store(page.data(), 2);

  const auto view = tinydb::LeafPageView::Open(page.data(), 2);
  ASSERT_TRUE(view.has_value()) << view.error().ToString();
  EXPECT_EQ(view->Count(), 3U);
  EXPECT_EQ(view->NextLeaf(), tinydb::HEADER_PAGE_ID);
  EXPECT_EQ(view->KeyAt(0), "alpha");
  EXPECT_EQ(view->ValueAt(1), "two");
  EXPECT_EQ(view->LowerBound(""), 0U);
  EXPECT_EQ(view->LowerBound("middle"), 1U);
  EXPECT_EQ(view->LowerBound("middle!"), 2U);
  EXPECT_EQ(view->LowerBound("zulu"), 3U);
  EXPECT_EQ(view->Get("omega"), std::optional<std::string_view>{"three"});
  EXPECT_EQ(view->Get("missing"), std::nullopt);

  const auto value = view->ValueAt(0);
  EXPECT_GE(value.data(), page.data());
  EXPECT_LT(value.data(), page.data() + page.size());
}

TEST(LeafPageViewTest, UsesUnsignedBytewiseKeyOrder) {
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::LeafNode{};
  const auto low = std::string(1, static_cast<char>(0x7F));
  const auto high = std::string(1, static_cast<char>(0x80));
  builder.Upsert(low, "low");
  builder.Upsert(high, "high");
  builder.Store(page.data(), 2);

  const auto view = tinydb::LeafPageView::Open(page.data(), 2).value();
  EXPECT_EQ(view.LowerBound(low), 0U);
  EXPECT_EQ(view.LowerBound(high), 1U);
  EXPECT_EQ(view.Get(high), std::optional<std::string_view>{"high"});
}

TEST(InternalPageViewTest, EqualKeysRouteToTheRightChild) {
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::InternalNode{2, "bravo", 3};
  builder.InsertSeparator("delta", 4);
  builder.InsertSeparator("hotel", 5);
  builder.Store(page.data(), 6);

  const auto view = tinydb::InternalPageView::Open(page.data(), 6);
  ASSERT_TRUE(view.has_value()) << view.error().ToString();
  EXPECT_EQ(view->SeparatorCount(), 3U);
  EXPECT_EQ(view->ChildAt(0), 2U);
  EXPECT_EQ(view->ChildAt(3), 5U);
  EXPECT_EQ(view->FindChildIndex("alpha"), 0U);
  EXPECT_EQ(view->FindChildIndex("bravo"), 1U);
  EXPECT_EQ(view->FindChildIndex("charlie"), 1U);
  EXPECT_EQ(view->FindChildIndex("delta"), 2U);
  EXPECT_EQ(view->FindChildIndex("zulu"), 3U);
}

TEST(PageViewTest, RejectsWrongTypeIdentityAndReservedBytes) {
  auto leaf_page = std::array<char, tinydb::PAGE_SIZE>{};
  auto leaf = tinydb::LeafNode{};
  leaf.Upsert("key", "value");
  leaf.Store(leaf_page.data(), 2);

  EXPECT_EQ(tinydb::InternalPageView::Open(leaf_page.data(), 2).error().Code(), tinydb::StatusCode::Corruption);
  EXPECT_EQ(tinydb::LeafPageView::Open(leaf_page.data(), 3).error().Code(), tinydb::StatusCode::Corruption);

  auto bytes = std::as_writable_bytes(std::span{leaf_page});
  const auto slot = tinydb::storage::GetLittleEndian<tinydb::slot_t>(bytes, tinydb::LEAF_HEADER_SIZE);
  ASSERT_TRUE(slot.has_value());
  bytes[*slot + tinydb::leaf_cell_offset::RESERVED] = std::byte{1};
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  EXPECT_EQ(tinydb::LeafPageView::Open(leaf_page.data(), 2).error().Code(), tinydb::StatusCode::Corruption);
}

TEST(PageViewTest, RejectsMalformedSlotsAsCorruption) {
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::LeafNode{};
  builder.Upsert("key", "value");
  builder.Store(page.data(), 2);

  auto bytes = std::as_writable_bytes(std::span{page});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(bytes, tinydb::LEAF_HEADER_SIZE, tinydb::slot_t{1}));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  const auto view = tinydb::LeafPageView::Open(page.data(), 2);
  ASSERT_FALSE(view.has_value());
  EXPECT_EQ(view.error().Code(), tinydb::StatusCode::Corruption);
}

}  // namespace
