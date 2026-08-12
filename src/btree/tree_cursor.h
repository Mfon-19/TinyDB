#pragma once

#include "btree/page_source.h"
#include "btree/page_view.h"
#include "util/check.h"

#include <tinydb/status.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb {

/*
** FORWARD TREE CURSOR
**
** Seek descends once, then iteration follows the leaf successor chain. The
** cursor owns one leaf lease at a time. Key and the leaf value descriptor
** borrow that page and expire on movement or destruction. Overflow payload
** pages are read only when an owning value copy is requested.
**
** Opening each successor validates both page-local structure and global chain
** order. Strict key boundaries detect every cycle containing a non-empty
** leaf; a temporary set is needed only while skipping empty leaves. Normal
** scans therefore carry no cycle-tracking allocation. Empty underfull leaves
** are legal and are skipped.
*/
class BTreeCursor {
 public:
  static auto First(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count) -> Result<BTreeCursor>;
  static auto Seek(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count,
                   std::string_view key) -> Result<BTreeCursor>;

  BTreeCursor(const BTreeCursor &) = delete;
  auto operator=(const BTreeCursor &) -> BTreeCursor & = delete;
  BTreeCursor(BTreeCursor &&) noexcept = default;
  auto operator=(BTreeCursor &&) noexcept -> BTreeCursor & = default;

  auto Valid() const -> bool { return leaf_.has_value() && index_ < leaf_->Count(); }

  auto Key() const -> std::string_view { return CurrentLeaf().KeyAt(index_); }

  auto Value() const -> LeafValueView { return CurrentLeaf().ValueAt(index_); }

  auto CopyValue() const -> Result<std::string>;
  auto First() -> Status;
  auto Seek(std::string_view key) -> Status;
  auto Next(std::optional<std::string_view> upper = std::nullopt) -> Status;
  void FinishScan() noexcept;

 private:
  BTreeCursor(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count)
      : pages_(pages),
        stream_(pages->BeginReadStream()),
        root_page_id_(root_page_id),
        logical_page_count_(logical_page_count) {}

  static auto Position(PageReader *pages, page_id_t root_page_id, page_id_t logical_page_count,
                       std::optional<std::string_view> lower_bound) -> Result<BTreeCursor>;
  auto Reset(std::optional<std::string_view> lower_bound) -> Status;
  auto OpenInitialLeaf(PageHandle page) -> Status;
  auto AdvanceToNonEmptyLeaf(page_id_t page_id, std::optional<std::string_view> upper = std::nullopt) -> Status;
  auto ValidateLeafId(page_id_t page_id) const -> Status;
  auto ValidateBoundary(const LeafPageView &next) const -> Status;
  void CancelRangeAdvice() noexcept;
  void PrimeSuccessor(page_id_t page_id, std::optional<std::string_view> upper) noexcept;
  auto CurrentLeaf() const -> const LeafPageView & {
    TINYDB_CHECK(leaf_.has_value(), "tree cursor has no current leaf");
    return *leaf_;
  }

  PageReader *pages_;
  PageReadStream stream_;
  page_id_t root_page_id_;
  page_id_t logical_page_count_;
  PageHandle page_;
  std::optional<LeafPageView> leaf_;
  std::size_t index_{0};
  std::size_t range_advances_{0};
  std::vector<page_id_t> planned_successors_;
  std::size_t planned_successor_index_{0};
  bool range_planning_disabled_{false};
  bool successor_primed_{false};
};

}  // namespace tinydb
