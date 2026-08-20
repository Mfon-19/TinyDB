#pragma once

#include "btree/page_format.h"
#include "util/check.h"

#include <tinydb/status.h>
#include "storage/page.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

#include "btree/value.h"

namespace tinydb {

namespace page_view_detail {

/*
** PageView::Open has already proved every fixed field below lies inside its
** immutable page. Keep hot accessors free of repeated bounds/optional work.
*/
template <typename Integer>
auto LoadLittleEndian(const char *page, std::size_t offset) noexcept -> Integer {
  static_assert(std::is_unsigned_v<Integer>);
  auto value = Integer{};
  std::memcpy(&value, page + offset, sizeof(value));
  if constexpr (std::endian::native == std::endian::big) {
    return std::byteswap(value);
  } else {
    static_assert(std::endian::native == std::endian::little);
    return value;
  }
}

}  // namespace page_view_detail
/*
** BORROWED PAGE VIEWS
**
** Open establishes or reuses page-local structural validation. A raw
** persistent page crosses the checksum and structural validators here, while
** a transaction-private PageHandle may already carry the builder's structural
** proof. Subsequent accessors borrow keys and values directly from the encoded
** slot directory without allocating or decoding the complete node. The
** PageHandle owning page_ must outlive the view and keep its bytes immutable.
**
** Leaf LowerBound returns the first key not less than the target. Internal
** FindChildIndex implements upper_bound because separator equality belongs to
** the child on the separator's right.
*/
class LeafPageView {
 public:
  static auto Open(const char *page, page_id_t expected_page_id) -> Result<LeafPageView>;
  static auto Open(const PageHandle &page) -> Result<LeafPageView>;

  auto Count() const -> std::size_t { return cell_count_; }
  auto NextLeaf() const -> page_id_t { return next_leaf_; }

  auto KeyAt(std::size_t index) const -> std::string_view {
    const auto offset = CellOffset(index);
    const auto key_bytes =
        page_view_detail::LoadLittleEndian<std::uint16_t>(page_, offset + leaf_cell_offset::KEY_BYTES);
    return {page_ + offset + LEAF_CELL_HEADER_SIZE, key_bytes};
  }

  auto ValueAt(std::size_t index) const -> LeafValueView {
    const auto offset = CellOffset(index);
    const auto key_bytes =
        page_view_detail::LoadLittleEndian<std::uint16_t>(page_, offset + leaf_cell_offset::KEY_BYTES);
    const auto value_bytes =
        page_view_detail::LoadLittleEndian<std::uint16_t>(page_, offset + leaf_cell_offset::VALUE_BYTES);
    const auto value_offset = offset + LEAF_CELL_HEADER_SIZE + key_bytes;
    const auto kind = static_cast<LeafValueKind>(page_[offset + leaf_cell_offset::VALUE_KIND]);
    if (kind == LeafValueKind::Inline) {
      return LeafValueView::Inline(std::string_view{page_ + value_offset, value_bytes});
    }

    return LeafValueView::Overflow(OverflowValueDescriptor{
        .total_value_bytes = page_view_detail::LoadLittleEndian<std::uint64_t>(
            page_, value_offset + overflow_descriptor_offset::TOTAL_VALUE_BYTES),
        .first_page_id = page_view_detail::LoadLittleEndian<page_id_t>(
            page_, value_offset + overflow_descriptor_offset::FIRST_PAGE_ID),
    });
  }

  auto LowerBound(std::string_view key) const -> std::size_t;
  auto Get(std::string_view key) const -> std::optional<LeafValueView>;

 private:
  static auto OpenValidated(const char *page, std::uint16_t raw_type) -> Result<LeafPageView>;

  LeafPageView(const char *page, std::uint16_t cell_count, page_id_t next_leaf)
      : page_(page), cell_count_(cell_count), next_leaf_(next_leaf) {}

  auto CellOffset(std::size_t index) const -> std::size_t {
    TINYDB_CHECK(index < cell_count_, "leaf record index out of range");
    return page_view_detail::LoadLittleEndian<slot_t>(page_, LEAF_HEADER_SIZE + index * SLOT_SIZE);
  }

  const char *page_;
  std::uint16_t cell_count_;
  page_id_t next_leaf_;
};

class InternalPageView {
 public:
  static auto Open(const char *page, page_id_t expected_page_id) -> Result<InternalPageView>;
  static auto Open(const PageHandle &page) -> Result<InternalPageView>;

  auto SeparatorCount() const -> std::size_t { return separator_count_; }

  auto KeyAt(std::size_t index) const -> std::string_view {
    const auto offset = CellOffset(index);
    const auto key_bytes =
        page_view_detail::LoadLittleEndian<std::uint16_t>(page_, offset + internal_cell_offset::KEY_BYTES);
    return {page_ + offset + INTERNAL_CELL_HEADER_SIZE, key_bytes};
  }

  auto ChildAt(std::size_t child_index) const -> page_id_t {
    // Child zero is LINK; child i>0 belongs to separator i-1.
    TINYDB_CHECK(child_index <= separator_count_, "internal child index out of range");
    return child_index == 0 ? first_child_ : RightChildAt(child_index - 1);
  }

  auto FindChildIndex(std::string_view key) const -> std::size_t;

 private:
  static auto OpenValidated(const char *page, std::uint16_t raw_type) -> Result<InternalPageView>;

  InternalPageView(const char *page, std::uint16_t separator_count, page_id_t first_child)
      : page_(page), separator_count_(separator_count), first_child_(first_child) {}

  auto CellOffset(std::size_t index) const -> std::size_t {
    TINYDB_CHECK(index < separator_count_, "internal separator index out of range");
    return page_view_detail::LoadLittleEndian<slot_t>(page_, INTERNAL_HEADER_SIZE + index * SLOT_SIZE);
  }

  auto RightChildAt(std::size_t index) const -> page_id_t {
    const auto offset = CellOffset(index);
    return page_view_detail::LoadLittleEndian<page_id_t>(page_, offset + internal_cell_offset::RIGHT_CHILD);
  }

  const char *page_;
  std::uint16_t separator_count_;
  page_id_t first_child_;
};

}  // namespace tinydb
