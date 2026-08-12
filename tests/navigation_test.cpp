#include <gtest/gtest.h>

#include "btree/internal_page_builder.h"
#include "btree/leaf_page_builder.h"
#include "btree/navigation.h"
#include "btree/page_source.h"
#include "btree/tree_cursor.h"
#include "storage/page_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using PageBytes = std::array<char, tinydb::PAGE_SIZE>;

class RecordingPageReader final : public tinydb::PageReader {
 public:
  void Add(tinydb::page_id_t page_id, const PageBytes &bytes) {
    pages_.emplace(page_id, std::make_shared<const PageBytes>(bytes));
  }

  void AddLeaf(tinydb::page_id_t page_id, std::string_view key) {
    auto builder = tinydb::LeafPageBuilder{};
    static_cast<void>(builder.Upsert(key, tinydb::LeafValueView::Inline("value")));
    AddLeaf(page_id, builder);
  }

  void AddLeaf(tinydb::page_id_t page_id, const tinydb::LeafPageBuilder &builder) {
    auto page = PageBytes{};
    builder.Store(page.data(), page_id);
    tinydb::storage::FinalizeDataPage(std::as_writable_bytes(std::span{page}));
    Add(page_id, page);
  }

  void AddInternal(tinydb::page_id_t page_id, tinydb::page_id_t first_child,
                   std::initializer_list<std::pair<std::string_view, tinydb::page_id_t>> separators) {
    ASSERT_FALSE(separators.size() == 0);
    const auto *entry = separators.begin();
    auto builder = tinydb::InternalPageBuilder(first_child, entry->first, entry->second);
    for (++entry; entry != separators.end(); ++entry) {
      builder.InsertSeparator(entry->first, entry->second);
    }
    auto page = PageBytes{};
    builder.Store(page.data(), page_id);
    tinydb::storage::FinalizeDataPage(std::as_writable_bytes(std::span{page}));
    Add(page_id, page);
  }

  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    reads_.push_back(page_id);
    const auto found = pages_.find(page_id);
    if (found == pages_.end()) {
      return std::unexpected(tinydb::Status::IoError("test page is unavailable"));
    }
    auto keepalive = std::static_pointer_cast<const void>(found->second);
    return tinydb::PageHandle(page_id, found->second->data(), std::move(keepalive));
  }

  auto BeginReadStream() -> tinydb::PageReadStream override {
    const auto read = [](void *owner, const std::shared_ptr<void> &, tinydb::page_id_t page_id) {
      return static_cast<RecordingPageReader *>(owner)->Read(page_id);
    };
    const auto prime = [](void *owner, const std::shared_ptr<void> &,
                          std::span<const tinydb::page_id_t> page_ids) noexcept {
      auto *reader = static_cast<RecordingPageReader *>(owner);
      try {
        reader->plans_.emplace_back(page_ids.begin(), page_ids.end());
      } catch (...) {
        reader->plan_capture_failed_ = true;
      }
    };
    const auto close = [](void *owner, const std::shared_ptr<void> &) noexcept {
      ++static_cast<RecordingPageReader *>(owner)->close_count_;
    };
    return {this, std::make_shared<int>(0), read, prime, close};
  }

  auto Reads() const -> const std::vector<tinydb::page_id_t> & { return reads_; }
  auto Plans() const -> const std::vector<std::vector<tinydb::page_id_t>> & { return plans_; }
  auto CloseCount() const -> std::size_t { return close_count_; }
  auto PlanCaptureFailed() const -> bool { return plan_capture_failed_; }
  void ClearReads() { reads_.clear(); }

 private:
  std::map<tinydb::page_id_t, std::shared_ptr<const PageBytes>> pages_;
  std::vector<tinydb::page_id_t> reads_;
  std::vector<std::vector<tinydb::page_id_t>> plans_;
  std::size_t close_count_{0};
  bool plan_capture_failed_{false};
};

class PlainPageReader final : public tinydb::PageReader {
 public:
  PlainPageReader() {
    tinydb::LeafPageBuilder{}.Store(page_.data(), 2);
    tinydb::storage::FinalizeDataPage(std::as_writable_bytes(std::span{page_}));
  }

  auto Read(tinydb::page_id_t page_id) -> tinydb::Result<tinydb::PageHandle> override {
    ++reads_;
    if (page_id != 2) {
      return std::unexpected(tinydb::Status::IoError("test page is unavailable"));
    }
    return tinydb::PageHandle(page_id, page_.data(), {});
  }

  auto ReadCount() const -> std::size_t { return reads_; }

 private:
  PageBytes page_{};
  std::size_t reads_{0};
};

void AddBoundedTree(RecordingPageReader &pages) {
  // The path to page 19 is 2, 66, 19. Page 23 is its next leaf.
  pages.AddInternal(2, 66, {{"m", 80}, {"x", 90}});
  pages.AddInternal(66, 7, {{"g", 19}, {"j", 23}});
  pages.AddInternal(80, 5, {{"q", 30}});
  pages.AddLeaf(19, "hotel");
}

auto AddThreeLeafTree(RecordingPageReader &pages, bool matching_root) -> std::string {
  auto left = tinydb::LeafPageBuilder{};
  for (const auto *const key :
       {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima"}) {
    static_cast<void>(left.Upsert(key, tinydb::LeafValueView::Inline("value")));
  }
  auto first_split = left.Split(19, false);
  auto middle = std::move(first_split.right);
  auto second_split = middle.Split(23, false);
  auto right = std::move(second_split.right);
  const auto first_separator = std::move(first_split.separator);
  const auto second_separator = std::move(second_split.separator);
  pages.AddLeaf(7, left);
  pages.AddLeaf(19, middle);
  pages.AddLeaf(23, right);
  if (matching_root) {
    pages.AddInternal(2, 7, {{first_separator, 19}, {second_separator, 23}});
  } else {
    pages.AddInternal(2, 7, {{first_separator, 23}});
  }
  return first_separator;
}

void AddEarlyEofTree(RecordingPageReader &pages) {
  auto left = tinydb::LeafPageBuilder{};
  for (const auto *const key :
       {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet"}) {
    static_cast<void>(left.Upsert(key, tinydb::LeafValueView::Inline("value")));
  }
  auto split = left.Split(19, false);
  const auto first_separator = std::move(split.separator);
  pages.AddLeaf(7, left);
  pages.AddLeaf(19, split.right);
  pages.AddLeaf(23, "zulu");
  pages.AddInternal(2, 7, {{first_separator, 19}, {"zulu", 23}});
}

TEST(PageReadStream, DefaultReaderIsPassThrough) {
  auto pages = PlainPageReader{};
  auto stream = pages.BeginReadStream();
  EXPECT_FALSE(stream.AcceptsExactPagePlan());
  const auto plan = std::array<tinydb::page_id_t, 1>{2};
  stream.PrimeExactPages(plan);
  EXPECT_TRUE(stream.Read(2).has_value());
  stream.CancelAdvice();
  EXPECT_TRUE(stream.Read(2).has_value());
  EXPECT_EQ(pages.ReadCount(), 2U);
}

TEST(Navigation, SuccessorPlanIsExactBoundedAndInternalOnly) {
  auto pages = RecordingPageReader{};
  AddBoundedTree(pages);

  const auto first = tinydb::PlanLeafPageSuccessorsForReadAhead(&pages, 2, 19, 100, "hotel", 1);
  EXPECT_EQ(first, (std::vector<tinydb::page_id_t>{23}));
  EXPECT_EQ(pages.Reads(), (std::vector<tinydb::page_id_t>{2, 66, 19, 66}));

  pages.ClearReads();
  const auto next = tinydb::PlanLeafPageSuccessorsForReadAhead(&pages, 2, 19, 100, "hotel", 2);
  EXPECT_EQ(next, (std::vector<tinydb::page_id_t>{23, 5}));
  EXPECT_EQ(pages.Reads(), (std::vector<tinydb::page_id_t>{2, 66, 19, 66, 2, 80}));
  EXPECT_EQ(std::ranges::find(pages.Reads(), 23), pages.Reads().end());
  EXPECT_EQ(std::ranges::find(pages.Reads(), 5), pages.Reads().end());
}

TEST(Navigation, SuccessorPlanStopsAtExclusiveUpperBound) {
  auto pages = RecordingPageReader{};
  AddBoundedTree(pages);

  EXPECT_TRUE(tinydb::PlanLeafPageSuccessorsForReadAhead(&pages, 2, 19, 100, "hotel", 8, "j").empty());
  EXPECT_EQ(tinydb::PlanLeafPageSuccessorsForReadAhead(&pages, 2, 19, 100, "hotel", 8, "k"),
            (std::vector<tinydb::page_id_t>{23}));
  EXPECT_EQ(tinydb::PlanLeafPageSuccessorsForReadAhead(&pages, 2, 19, 100, "hotel", 8, "n"),
            (std::vector<tinydb::page_id_t>{23, 5}));
}

TEST(Navigation, UnprovableSuccessorPlanReturnsEmpty) {
  auto pages = RecordingPageReader{};
  pages.AddInternal(2, 66, {{"m", 80}});
  pages.AddInternal(66, 7, {{"g", 19}});
  pages.AddLeaf(19, "hotel");

  const auto planned = tinydb::PlanLeafPageSuccessorsForReadAhead(&pages, 2, 19, 100, "hotel", 8);
  EXPECT_TRUE(planned.empty());
  EXPECT_EQ(pages.Reads().back(), 80U);
}

TEST(TreeCursor, ForwardMovesStartOneBoundedPlan) {
  auto pages = RecordingPageReader{};
  AddThreeLeafTree(pages, true);

  auto cursor = tinydb::BTreeCursor::Seek(&pages, 2, 100, "alpha");
  ASSERT_TRUE(cursor.has_value()) << cursor.error().ToString();
  EXPECT_TRUE(pages.Plans().empty());
  ASSERT_TRUE(cursor->Next().Ok());
  EXPECT_TRUE(pages.Plans().empty());
  ASSERT_TRUE(cursor->Next().Ok());
  ASSERT_FALSE(pages.PlanCaptureFailed());
  ASSERT_EQ(pages.Plans().size(), 1U);
  EXPECT_EQ(pages.Plans().front(), (std::vector<tinydb::page_id_t>{19, 23}));
}

TEST(TreeCursor, PlanMismatchFallsBackToLeafChain) {
  auto pages = RecordingPageReader{};
  const auto next_key = AddThreeLeafTree(pages, false);

  auto cursor = tinydb::BTreeCursor::Seek(&pages, 2, 100, "alpha");
  ASSERT_TRUE(cursor.has_value()) << cursor.error().ToString();
  ASSERT_TRUE(cursor->Next().Ok());
  ASSERT_TRUE(cursor->Next().Ok());
  // The plan's first leaf disagrees with the authenticated link, so the
  // cursor issues no advice and the leaf chain remains authoritative.
  EXPECT_TRUE(pages.Plans().empty());

  while (cursor->Valid() && cursor->Key() != next_key) {
    ASSERT_TRUE(cursor->Next().Ok());
  }
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->Key(), next_key);
}

TEST(TreeCursor, UpperBoundPreventsSuccessorPlan) {
  auto pages = RecordingPageReader{};
  AddThreeLeafTree(pages, true);

  auto cursor = tinydb::BTreeCursor::Seek(&pages, 2, 100, "alpha");
  ASSERT_TRUE(cursor.has_value()) << cursor.error().ToString();
  ASSERT_TRUE(cursor->Next("delta").Ok());
  ASSERT_TRUE(cursor->Next("delta").Ok());
  EXPECT_TRUE(pages.Plans().empty());
}

TEST(TreeCursor, EarlyEndClosesUnusedPlan) {
  auto pages = RecordingPageReader{};
  AddEarlyEofTree(pages);

  auto cursor = tinydb::BTreeCursor::Seek(&pages, 2, 100, "alpha");
  ASSERT_TRUE(cursor.has_value()) << cursor.error().ToString();
  while (cursor->Valid()) {
    ASSERT_TRUE(cursor->Next().Ok());
  }

  ASSERT_EQ(pages.Plans().size(), 1U);
  EXPECT_EQ(pages.Plans().front(), (std::vector<tinydb::page_id_t>{19, 23}));
  EXPECT_EQ(pages.CloseCount(), 1U);
}

}  // namespace
