#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "btree/value.h"

namespace tinydb {

/*
** BORROWED PAGE VIEWS
**
** Open performs full persistent validation once. Subsequent accessors borrow
** keys and values directly from the encoded slot directory without allocating
** or decoding the complete node. The PageHandle owning page_ must outlive the
** view and keep its bytes immutable.
**
** Leaf LowerBound returns the first key not less than the target. Internal
** FindChildIndex implements upper_bound because separator equality belongs to
** the child on the separator's right.
*/
class LeafPageView {
 public:
  static auto Open(const char *page, page_id_t expected_page_id) -> Result<LeafPageView>;

  auto Count() const -> std::size_t { return cell_count_; }
  auto NextLeaf() const -> page_id_t { return next_leaf_; }

  auto KeyAt(std::size_t index) const -> std::string_view;
  auto ValueAt(std::size_t index) const -> LeafValueView;
  auto LowerBound(std::string_view key) const -> std::size_t;
  auto Get(std::string_view key) const -> std::optional<LeafValueView>;

 private:
  LeafPageView(const char *page, std::uint16_t cell_count, page_id_t next_leaf)
      : page_(page), cell_count_(cell_count), next_leaf_(next_leaf) {}

  auto CellOffset(std::size_t index) const -> std::size_t;

  const char *page_;
  std::uint16_t cell_count_;
  page_id_t next_leaf_;
};

/* Borrowed routing-page decoder. Equal keys route right. */
class InternalPageView {
 public:
  static auto Open(const char *page, page_id_t expected_page_id) -> Result<InternalPageView>;

  auto SeparatorCount() const -> std::size_t { return separator_count_; }
  auto KeyAt(std::size_t index) const -> std::string_view;
  auto ChildAt(std::size_t child_index) const -> page_id_t;
  auto FindChildIndex(std::string_view key) const -> std::size_t;

 private:
  InternalPageView(const char *page, std::uint16_t separator_count, page_id_t first_child)
      : page_(page), separator_count_(separator_count), first_child_(first_child) {}

  auto CellOffset(std::size_t index) const -> std::size_t;
  auto RightChildAt(std::size_t index) const -> page_id_t;

  const char *page_;
  std::uint16_t separator_count_;
  page_id_t first_child_;
};

}  // namespace tinydb
