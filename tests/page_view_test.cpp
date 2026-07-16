#include <gtest/gtest.h>

#include "btree/internal_page_builder.h"
#include "btree/leaf_page_builder.h"
#include "btree/page_format.h"
#include "btree/page_view.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace {

TEST(LeafPageViewTest, SearchesEncodedRecordsWithoutOwningThem) {
  auto page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::LeafPageBuilder{};
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
  auto builder = tinydb::LeafPageBuilder{};
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
  auto builder = tinydb::InternalPageBuilder{2, "bravo", 3};
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
  auto leaf = tinydb::LeafPageBuilder{};
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
  auto builder = tinydb::LeafPageBuilder{};
  builder.Upsert("key", "value");
  builder.Store(page.data(), 2);

  auto bytes = std::as_writable_bytes(std::span{page});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(bytes, tinydb::LEAF_HEADER_SIZE, tinydb::slot_t{1}));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  const auto view = tinydb::LeafPageView::Open(page.data(), 2);
  ASSERT_FALSE(view.has_value());
  EXPECT_EQ(view.error().Code(), tinydb::StatusCode::Corruption);
}

TEST(PageViewTest, RejectsInvalidLinksAndSlotOrdering) {
  auto internal_page = std::array<char, tinydb::PAGE_SIZE>{};
  auto internal = tinydb::InternalPageBuilder{2, "bravo", 3};
  internal.InsertSeparator("delta", 4);
  internal.Store(internal_page.data(), 5);

  // The header link is the mandatory leftmost child of an internal page.
  auto bytes = std::as_writable_bytes(std::span{internal_page});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(bytes, tinydb::node_page_offset::LINK, tinydb::HEADER_PAGE_ID));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  EXPECT_EQ(tinydb::InternalPageView::Open(internal_page.data(), 5).error().Code(),
            tinydb::StatusCode::Corruption);

  internal.Store(internal_page.data(), 5);
  bytes = std::as_writable_bytes(std::span{internal_page});
  const auto first_slot = tinydb::storage::GetLittleEndian<tinydb::slot_t>(bytes, tinydb::INTERNAL_HEADER_SIZE);
  const auto second_slot =
      tinydb::storage::GetLittleEndian<tinydb::slot_t>(bytes, tinydb::INTERNAL_HEADER_SIZE + tinydb::SLOT_SIZE);
  ASSERT_TRUE(first_slot.has_value() && second_slot.has_value());
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(bytes, tinydb::INTERNAL_HEADER_SIZE, *second_slot));
  ASSERT_TRUE(
      tinydb::storage::PutLittleEndian(bytes, tinydb::INTERNAL_HEADER_SIZE + tinydb::SLOT_SIZE, *first_slot));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  EXPECT_EQ(tinydb::InternalPageView::Open(internal_page.data(), 5).error().Code(),
            tinydb::StatusCode::Corruption);

  auto leaf_page = std::array<char, tinydb::PAGE_SIZE>{};
  tinydb::LeafPageBuilder{}.Store(leaf_page.data(), 6);
  bytes = std::as_writable_bytes(std::span{leaf_page});
  ASSERT_TRUE(tinydb::storage::PutLittleEndian(bytes, tinydb::node_page_offset::LINK,
                                               tinydb::SUPERBLOCK_B_PAGE_ID));
  ASSERT_TRUE(tinydb::storage::FinalizeDataPage(bytes).Ok());
  EXPECT_EQ(tinydb::LeafPageView::Open(leaf_page.data(), 6).error().Code(), tinydb::StatusCode::Corruption);
}

TEST(PageViewTest, DecoderFuzzNeverExposesUncheckedSlotsOrCells) {
  auto seed_page = std::array<char, tinydb::PAGE_SIZE>{};
  auto builder = tinydb::LeafPageBuilder{};
  for (int index = 0; index < 24; ++index) {
    builder.Upsert("key-" + std::to_string(index + 100), std::string(static_cast<std::size_t>(index), 'v'));
  }
  builder.Store(seed_page.data(), 2);

  // Recompute the checksum after each mutation so the fuzzer reaches the
  // structural decoder instead of being rejected at the outer checksum on
  // every iteration. A successful Open must make every accessor safe.
  auto rng = std::mt19937{0xB17E5U};
  auto offset = std::uniform_int_distribution<std::size_t>{0, tinydb::PAGE_SIZE - 1};
  auto byte = std::uniform_int_distribution<unsigned>{0, 255};
  auto mutation_count = std::uniform_int_distribution<int>{1, 6};
  for (int iteration = 0; iteration < 2000; ++iteration) {
    auto page = seed_page;
    for (int mutation = 0; mutation < mutation_count(rng); ++mutation) {
      page[offset(rng)] = static_cast<char>(byte(rng));
    }
    ASSERT_TRUE(tinydb::storage::FinalizeDataPage(std::as_writable_bytes(std::span{page})).Ok());

    const auto view = tinydb::LeafPageView::Open(page.data(), 2);
    if (!view) {
      EXPECT_TRUE(view.error().Code() == tinydb::StatusCode::Corruption ||
                  view.error().Code() == tinydb::StatusCode::UnsupportedFormat);
      continue;
    }
    for (std::size_t index = 0; index < view->Count(); ++index) {
      static_cast<void>(view->KeyAt(index));
      static_cast<void>(view->ValueAt(index));
    }
    static_cast<void>(view->LowerBound("probe"));
  }
}

}  // namespace
