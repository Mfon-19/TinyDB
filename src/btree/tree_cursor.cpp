#include "btree/tree_cursor.h"

#include "btree/navigation.h"
#include "btree/value_storage.h"
#include "txn/contract.h"

#include "storage/page.h"

#include <expected>
#include <limits>
#include <unordered_set>
#include <utility>

namespace tinydb {
namespace {

constexpr auto RANGE_READ_AHEAD_PAGES = std::size_t{128};

}  // namespace

auto BTreeCursor::First(PageReader *pages, page_id_t root_page_id,
                        page_id_t logical_page_count) -> Result<BTreeCursor> {
  return Position(pages, root_page_id, logical_page_count, std::nullopt);
}

auto BTreeCursor::Seek(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count,
                       std::string_view key) -> Result<BTreeCursor> {
  return Position(pages, root_page_id, logical_page_count, key);
}

auto BTreeCursor::CopyValue() const -> Result<std::string> { return tinydb::CopyValue(pages_, Value()); }

auto BTreeCursor::First() -> Status { return Reset(std::nullopt); }

auto BTreeCursor::Seek(std::string_view key) -> Status { return Reset(key); }

auto BTreeCursor::Next(std::optional<std::string_view> upper) -> Status {
  TINYDB_CHECK(Valid(), "advancing an invalid tree cursor");
  if (range_advances_ != std::numeric_limits<std::size_t>::max()) {
    ++range_advances_;
  }
  // Two moves prove that the caller performs a scan. This delay prevents
  // read-ahead work for a cursor that reads one row.
  if (range_advances_ >= 2U && !successor_primed_) {
    PrimeSuccessor(CurrentLeaf().NextLeaf(), upper);
  }
  ++index_;
  if (index_ < CurrentLeaf().Count()) {
    return {};
  }
  return AdvanceToNonEmptyLeaf(CurrentLeaf().NextLeaf(), upper);
}

void BTreeCursor::FinishScan() noexcept {
  CancelRangeAdvice();
  leaf_.reset();
  page_ = PageHandle{};
  index_ = 0;
}

auto BTreeCursor::Position(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count,
                           std::optional<std::string_view> lower_bound) -> Result<BTreeCursor> {
  auto page =
      lower_bound.has_value() ? FindLeaf(pages, root_page_id, *lower_bound) : FindFirstLeaf(pages, root_page_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }

  auto cursor = BTreeCursor(pages, root_page_id, logical_page_count);
  if (auto status = cursor.OpenInitialLeaf(std::move(*page)); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (lower_bound.has_value()) {
    cursor.index_ = cursor.CurrentLeaf().LowerBound(*lower_bound);
  }
  if (cursor.Valid()) {
    return cursor;
  }
  // Keep this lease alive while opening the successor so its last key remains
  // available for the global ordering check without an owning copy.
  const auto next_leaf = cursor.CurrentLeaf().NextLeaf();
  if (auto status = cursor.AdvanceToNonEmptyLeaf(next_leaf); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return cursor;
}

auto BTreeCursor::Reset(std::optional<std::string_view> lower_bound) -> Status {
  auto replacement = Position(pages_, root_page_id_, logical_page_count_, lower_bound);
  if (!replacement) {
    return replacement.error();
  }
  *this = std::move(*replacement);
  return {};
}

auto BTreeCursor::OpenInitialLeaf(PageHandle page) -> Status {
  if (auto status = ValidateLeafId(page.Id()); !status.Ok()) {
    return status;
  }
  auto leaf = LeafPageView::Open(page);
  if (!leaf) {
    return leaf.error();
  }
  // Adopt the new lease before retaining the borrowed view. PageHandle move
  // preserves the address returned by the page source.
  page_ = std::move(page);
  leaf_ = *leaf;
  index_ = 0;
  return {};
}

auto BTreeCursor::AdvanceToNonEmptyLeaf(page_id_t page_id, std::optional<std::string_view> upper) -> Status {
  // Keep the previous non-empty leaf pinned across sparse successors. Its last
  // key can then validate the next non-empty boundary without allocating.
  auto empty_pages = std::unordered_set<page_id_t>{};
  if (leaf_.has_value() && leaf_->Count() == 0) {
    empty_pages.insert(page_.Id());
  }
  while (page_id != HEADER_PAGE_ID) {
    if (auto status = ValidateLeafId(page_id); !status.Ok()) {
      CancelRangeAdvice();
      return status;
    }
    if (empty_pages.contains(page_id)) {
      CancelRangeAdvice();
      return Status::Corruption("leaf chain contains a cycle");
    }
    auto followed_plan = false;
    if (planned_successor_index_ < planned_successors_.size()) {
      // The authenticated leaf link remains authoritative. Use staged bytes
      // only when the plan predicts that exact link.
      if (planned_successors_[planned_successor_index_] == page_id) {
        followed_plan = true;
      } else {
        CancelRangeAdvice();
      }
    }
    auto page = stream_.Read(page_id);
    if (!page) {
      CancelRangeAdvice();
      return std::move(page).error();
    }
    if (followed_plan) {
      ++planned_successor_index_;
      if (planned_successor_index_ == planned_successors_.size()) {
        planned_successors_.clear();
        planned_successor_index_ = 0;
      }
    }
    auto leaf = LeafPageView::Open(*page);
    if (!leaf) {
      CancelRangeAdvice();
      return leaf.error();
    }
    if (leaf->Count() == 0) {
      empty_pages.insert(page_id);
      page_id = leaf->NextLeaf();
      if (range_advances_ >= 2U && planned_successor_index_ == planned_successors_.size()) {
        PrimeSuccessor(page_id, upper);
      }
      continue;
    }
    if (auto status = ValidateBoundary(*leaf); !status.Ok()) {
      CancelRangeAdvice();
      return status;
    }
    page_ = std::move(*page);
    leaf_ = *leaf;
    index_ = 0;
    successor_primed_ = planned_successor_index_ < planned_successors_.size();
    if (range_advances_ >= 2U && !successor_primed_) {
      PrimeSuccessor(CurrentLeaf().NextLeaf(), upper);
    }
    return {};
  }

  CancelRangeAdvice();
  page_ = PageHandle{};
  // End-of-chain is represented by an absent view, not a fabricated position.
  leaf_.reset();
  index_ = 0;
  return {};
}

void BTreeCursor::CancelRangeAdvice() noexcept {
  // A mismatch makes the remaining plan unprovable. Keep semantic reads
  // active, but disable more advice for this cursor position.
  planned_successors_.clear();
  planned_successor_index_ = 0;
  successor_primed_ = false;
  range_planning_disabled_ = true;
  stream_.CancelAdvice();
}

void BTreeCursor::PrimeSuccessor(page_id_t page_id, std::optional<std::string_view> upper) noexcept {
  successor_primed_ = true;
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count_) {
    return;
  }
  if (upper.has_value() && CurrentLeaf().Count() != 0U &&
      !txn::BytewiseLess{}(CurrentLeaf().KeyAt(CurrentLeaf().Count() - 1U), *upper)) {
    return;
  }
  if (planned_successor_index_ < planned_successors_.size()) {
    return;
  }
  if (range_planning_disabled_ || !Valid() || CurrentLeaf().NextLeaf() != page_id || !stream_.AcceptsExactPagePlan()) {
    return;
  }
  // Internal edges provide the batch. The first planned leaf must also match
  // the authenticated link from the current leaf, which stays authoritative;
  // a mismatch proves no plan and the cursor follows the chain without advice.
  auto pages = PlanLeafPageSuccessorsForReadAhead(pages_, root_page_id_, page_.Id(), logical_page_count_, Key(),
                                                  RANGE_READ_AHEAD_PAGES, upper);
  if (!pages.empty() && pages.front() == page_id) {
    planned_successors_ = std::move(pages);
    planned_successor_index_ = 0;
    stream_.PrimeExactPages(planned_successors_);
  } else if (!pages.empty()) {
    range_planning_disabled_ = true;
  }
}

auto BTreeCursor::ValidateLeafId(page_id_t page_id) const -> Status {
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count_) {
    return Status::Corruption("leaf page lies outside the logical page range");
  }
  return {};
}

auto BTreeCursor::ValidateBoundary(const LeafPageView &next) const -> Status {
  if (leaf_.has_value() && leaf_->Count() != 0 &&
      !txn::BytewiseLess{}(leaf_->KeyAt(leaf_->Count() - 1), next.KeyAt(0))) {
    return Status::Corruption("leaf chain is out of key order");
  }
  return {};
}

}  // namespace tinydb
