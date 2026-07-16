#pragma once

#include "btree/page_source.h"
#include "btree/page_view.h"

#include <tinydb/status.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace tinydb {

/*
** FORWARD TREE CURSOR
**
** Seek descends once, then iteration follows the leaf successor chain. The
** cursor owns one page lease at a time. Key and Value borrow that page and
** expire on Next or destruction.
**
** Opening each successor validates both page-local structure and global chain
** order. visited_ detects cycles even through empty leaves, for which no key
** boundary exists. Empty underfull leaves are legal and are skipped.
*/
class BTreeCursor {
 public:
  static auto First(PageReader *pages, page_id_t root_page_id) -> Result<BTreeCursor>;
  static auto Seek(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<BTreeCursor>;

  BTreeCursor(const BTreeCursor &) = delete;
  auto operator=(const BTreeCursor &) -> BTreeCursor & = delete;
  BTreeCursor(BTreeCursor &&) noexcept = default;
  auto operator=(BTreeCursor &&) noexcept -> BTreeCursor & = default;

  auto Valid() const -> bool { return leaf_.has_value() && index_ < leaf_->Count(); }
  auto Key() const -> std::string_view;
  auto Value() const -> std::string_view;
  auto Next() -> Status;

 private:
  explicit BTreeCursor(PageReader *pages) : pages_(pages) {}

  auto OpenLeaf(page_id_t page_id, std::size_t index) -> Status;
  auto AdvanceToNonEmptyLeaf(page_id_t page_id) -> Status;
  void RememberLastKey();

  PageReader *pages_;
  PageHandle page_;
  std::optional<LeafPageView> leaf_;
  std::size_t index_{0};
  std::optional<std::string> previous_last_key_;
  std::unordered_set<page_id_t> visited_;
};

}  // namespace tinydb
