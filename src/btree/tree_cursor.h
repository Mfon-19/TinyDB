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

// Forward leaf-chain cursor. Key and Value borrow the one page held by page_
// and expire on Next or destruction.
class BTreeCursor {
 public:
  static auto Seek(PageSource *pages, page_id_t root_page_id, std::string_view key) -> Result<BTreeCursor>;

  BTreeCursor(const BTreeCursor &) = delete;
  auto operator=(const BTreeCursor &) -> BTreeCursor & = delete;
  BTreeCursor(BTreeCursor &&) noexcept = default;
  auto operator=(BTreeCursor &&) noexcept -> BTreeCursor & = default;

  auto Valid() const -> bool { return leaf_.has_value() && index_ < leaf_->Count(); }
  auto Key() const -> std::string_view;
  auto Value() const -> std::string_view;
  auto Next() -> Status;

 private:
  explicit BTreeCursor(PageSource *pages) : pages_(pages) {}

  auto OpenLeaf(page_id_t page_id, std::size_t index) -> Status;
  auto AdvanceToNonEmptyLeaf(page_id_t page_id) -> Status;
  void RememberLastKey();

  PageSource *pages_;
  PageHandle page_;
  std::optional<LeafPageView> leaf_;
  std::size_t index_{0};
  std::optional<std::string> previous_last_key_;
  std::unordered_set<page_id_t> visited_;
};

}  // namespace tinydb
