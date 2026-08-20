#include "btree/page_view.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "txn/contract.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace tinydb {

auto LeafPageView::Open(const char *page, page_id_t expected_page_id) -> Result<LeafPageView> {
  if (auto status = ValidateTreePage(page, expected_page_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return OpenValidated(page, RawNodeType(page));
}

auto LeafPageView::Open(const PageHandle &page) -> Result<LeafPageView> {
  if (auto status = ValidateTreePage(page); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return OpenValidated(page.Data(), RawNodeType(page));
}

auto LeafPageView::OpenValidated(const char *page, std::uint16_t raw_type) -> Result<LeafPageView> {
  if (raw_type != static_cast<std::uint16_t>(NodeType::Leaf)) {
    return std::unexpected(Status::Corruption("page is not a leaf node"));
  }

  const auto cell_count = page_view_detail::LoadLittleEndian<std::uint16_t>(page, node_page_offset::CELL_COUNT);
  const auto next_leaf = page_view_detail::LoadLittleEndian<page_id_t>(page, node_page_offset::LINK);
  return LeafPageView(page, cell_count, next_leaf);
}

auto LeafPageView::LowerBound(std::string_view key) const -> std::size_t {
  const auto less = txn::BytewiseLess{};
  auto first = std::size_t{0};
  auto last = static_cast<std::size_t>(cell_count_);
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    if (less(KeyAt(middle), key)) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first;
}

/*
** Search for equality directly. Calling LowerBound and comparing its result
** would repeat the final key comparison on every successful point lookup.
*/
auto LeafPageView::Get(std::string_view key) const -> std::optional<LeafValueView> {
  auto first = std::size_t{0};
  auto last = static_cast<std::size_t>(cell_count_);
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    const auto order = txn::BytewiseCompare(KeyAt(middle), key);
    if (order < 0) {
      first = middle + 1;
    } else if (order > 0) {
      last = middle;
    } else {
      return ValueAt(middle);
    }
  }
  return std::nullopt;
}

auto InternalPageView::Open(const char *page, page_id_t expected_page_id) -> Result<InternalPageView> {
  if (auto status = ValidateTreePage(page, expected_page_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return OpenValidated(page, RawNodeType(page));
}

auto InternalPageView::Open(const PageHandle &page) -> Result<InternalPageView> {
  if (auto status = ValidateTreePage(page); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return OpenValidated(page.Data(), RawNodeType(page));
}

auto InternalPageView::OpenValidated(const char *page, std::uint16_t raw_type) -> Result<InternalPageView> {
  if (raw_type != static_cast<std::uint16_t>(NodeType::Internal)) {
    return std::unexpected(Status::Corruption("page is not an internal node"));
  }

  const auto separator_count = page_view_detail::LoadLittleEndian<std::uint16_t>(page, node_page_offset::CELL_COUNT);
  const auto first_child = page_view_detail::LoadLittleEndian<page_id_t>(page, node_page_offset::LINK);
  return InternalPageView(page, separator_count, first_child);
}

/*
** Return the upper_bound position of key. Separators are inclusive lower
** bounds for their right child, so equality belongs to child middle+1 rather
** than the child on its left.
*/
auto InternalPageView::FindChildIndex(std::string_view key) const -> std::size_t {
  auto first = std::size_t{0};
  auto last = static_cast<std::size_t>(separator_count_);
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    const auto order = txn::BytewiseCompare(key, KeyAt(middle));
    if (order > 0) {
      first = middle + 1;
    } else if (order < 0) {
      last = middle;
    } else {
      return middle + 1;
    }
  }
  return first;
}

}  // namespace tinydb
