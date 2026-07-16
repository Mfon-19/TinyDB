#include "btree/page_view.h"

#include <tinydb/check.h>

#include "btree/page_format.h"
#include "storage/encoding.h"
#include "txn/contract.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace tinydb {
namespace {

// Fixed-width fields use byte spans; record access returns char-based views.
auto Bytes(const char *page) -> std::span<const std::byte> { return std::as_bytes(std::span{page, PAGE_SIZE}); }

}  // namespace

auto LeafPageView::Open(const char *page, page_id_t expected_page_id) -> Result<LeafPageView> {
  // One full validation makes later accessors branch-free with respect to
  // persistent corruption. They retain only programmer-invariant checks.
  if (auto status = ValidateTreePage(page, expected_page_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (RawNodeType(page) != static_cast<std::uint16_t>(NodeType::Leaf)) {
    return std::unexpected(Status::Corruption("page is not a leaf node"));
  }

  const auto bytes = Bytes(page);
  const auto cell_count = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::CELL_COUNT);
  const auto next_leaf = storage::GetLittleEndian<page_id_t>(bytes, node_page_offset::LINK);
  // Missing fields now imply the supposedly immutable page changed after validation.
  TINYDB_CHECK(cell_count.has_value() && next_leaf.has_value(), "validated leaf header became unreadable");
  return LeafPageView(page, *cell_count, *next_leaf);
}

auto LeafPageView::CellOffset(std::size_t index) const -> std::size_t {
  TINYDB_CHECK(index < cell_count_, "leaf record index out of range");
  return *storage::GetLittleEndian<slot_t>(Bytes(page_), LEAF_HEADER_SIZE + index * SLOT_SIZE);
}

auto LeafPageView::KeyAt(std::size_t index) const -> std::string_view {
  const auto offset = CellOffset(index);
  const auto key_bytes = *storage::GetLittleEndian<std::uint16_t>(Bytes(page_), offset + leaf_cell_offset::KEY_BYTES);
  // The owning PageHandle bounds the returned slice's lifetime.
  return {page_ + offset + LEAF_CELL_HEADER_SIZE, key_bytes};
}

auto LeafPageView::ValueAt(std::size_t index) const -> LeafValueView {
  const auto offset = CellOffset(index);
  const auto bytes = Bytes(page_);
  const auto key_bytes = *storage::GetLittleEndian<std::uint16_t>(bytes, offset + leaf_cell_offset::KEY_BYTES);
  const auto value_bytes = *storage::GetLittleEndian<std::uint16_t>(bytes, offset + leaf_cell_offset::VALUE_BYTES);
  const auto value_offset = offset + LEAF_CELL_HEADER_SIZE + key_bytes;
  const auto kind = static_cast<LeafValueKind>(page_[offset + leaf_cell_offset::VALUE_KIND]);
  if (kind == LeafValueKind::Inline) {
    return LeafValueView::Inline(std::string_view{page_ + value_offset, value_bytes});
  }

  const auto total = *storage::GetLittleEndian<std::uint64_t>(
      bytes, value_offset + overflow_descriptor_offset::TOTAL_VALUE_BYTES);
  const auto first =
      *storage::GetLittleEndian<page_id_t>(bytes, value_offset + overflow_descriptor_offset::FIRST_PAGE_ID);
  const auto checksum = *storage::GetLittleEndian<std::uint32_t>(
      bytes, value_offset + overflow_descriptor_offset::VALUE_CHECKSUM);
  return LeafValueView::Overflow(OverflowValueDescriptor{
      .total_value_bytes = total,
      .first_page_id = first,
      .value_checksum = checksum,
  });
}

auto LeafPageView::LowerBound(std::string_view key) const -> std::size_t {
  // Search the encoded slot order without constructing keys.
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

auto LeafPageView::Get(std::string_view key) const -> std::optional<LeafValueView> {
  // LowerBound identifies the sole possible match.
  const auto index = LowerBound(key);
  if (index == cell_count_ || KeyAt(index) != key) {
    return std::nullopt;
  }
  return ValueAt(index);
}

auto InternalPageView::Open(const char *page, page_id_t expected_page_id) -> Result<InternalPageView> {
  if (auto status = ValidateTreePage(page, expected_page_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (RawNodeType(page) != static_cast<std::uint16_t>(NodeType::Internal)) {
    return std::unexpected(Status::Corruption("page is not an internal node"));
  }

  const auto bytes = Bytes(page);
  const auto separator_count = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::CELL_COUNT);
  const auto first_child = storage::GetLittleEndian<page_id_t>(bytes, node_page_offset::LINK);
  TINYDB_CHECK(separator_count.has_value() && first_child.has_value(), "validated internal header became unreadable");
  return InternalPageView(page, *separator_count, *first_child);
}

auto InternalPageView::CellOffset(std::size_t index) const -> std::size_t {
  TINYDB_CHECK(index < separator_count_, "internal separator index out of range");
  return *storage::GetLittleEndian<slot_t>(Bytes(page_), INTERNAL_HEADER_SIZE + index * SLOT_SIZE);
}

auto InternalPageView::KeyAt(std::size_t index) const -> std::string_view {
  const auto offset = CellOffset(index);
  const auto key_bytes =
      *storage::GetLittleEndian<std::uint16_t>(Bytes(page_), offset + internal_cell_offset::KEY_BYTES);
  return {page_ + offset + INTERNAL_CELL_HEADER_SIZE, key_bytes};
}

auto InternalPageView::RightChildAt(std::size_t index) const -> page_id_t {
  const auto offset = CellOffset(index);
  return *storage::GetLittleEndian<page_id_t>(Bytes(page_), offset + internal_cell_offset::RIGHT_CHILD);
}

auto InternalPageView::ChildAt(std::size_t child_index) const -> page_id_t {
  // Child zero is LINK; child i>0 belongs to separator i-1.
  TINYDB_CHECK(child_index <= separator_count_, "internal child index out of range");
  return child_index == 0 ? first_child_ : RightChildAt(child_index - 1);
}

auto InternalPageView::FindChildIndex(std::string_view key) const -> std::size_t {
  // upper_bound implements the inclusive lower bound of every right child:
  // child i owns [separator i-1, separator i).
  const auto less = txn::BytewiseLess{};
  auto first = std::size_t{0};
  auto last = static_cast<std::size_t>(separator_count_);
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    if (!less(key, KeyAt(middle))) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first;
}

}  // namespace tinydb
