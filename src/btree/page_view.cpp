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

// Views retain the original char buffer because their public slices are
// string_view. Convert to bytes only while reading fixed-width metadata.
auto Bytes(const char *page) -> std::span<const std::byte> { return std::as_bytes(std::span{page, PAGE_SIZE}); }

}  // namespace

auto LeafPageView::Open(const char *page, page_id_t expected_page_id) -> Result<LeafPageView> {
  // Establish every structural invariant once at the boundary. Accessors may
  // then use checked offsets without repeating corruption branches for every
  // key comparison in a binary search.
  if (auto status = ValidateTreePage(page, expected_page_id); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  if (RawNodeType(page) != static_cast<std::uint16_t>(NodeType::Leaf)) {
    return std::unexpected(Status::Corruption("page is not a leaf node"));
  }

  const auto bytes = Bytes(page);
  const auto cell_count = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::CELL_COUNT);
  const auto next_leaf = storage::GetLittleEndian<page_id_t>(bytes, node_page_offset::LINK);
  // ValidateTreePage already proved these fields exist. Failure here would mean
  // the immutable bytes changed between validation and view construction.
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
  // The returned view points directly into the encoded cell. Its lifetime is
  // bounded by the pin that owns page_.
  return {page_ + offset + LEAF_CELL_HEADER_SIZE, key_bytes};
}

auto LeafPageView::ValueAt(std::size_t index) const -> std::string_view {
  const auto offset = CellOffset(index);
  const auto bytes = Bytes(page_);
  const auto key_bytes = *storage::GetLittleEndian<std::uint16_t>(bytes, offset + leaf_cell_offset::KEY_BYTES);
  const auto value_bytes = *storage::GetLittleEndian<std::uint16_t>(bytes, offset + leaf_cell_offset::VALUE_BYTES);
  return {page_ + offset + LEAF_CELL_HEADER_SIZE + key_bytes, value_bytes};
}

auto LeafPageView::LowerBound(std::string_view key) const -> std::size_t {
  // Slots are already in key order, so binary search needs only O(log n) cell
  // header reads and constructs no owning key strings.
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

auto LeafPageView::Get(std::string_view key) const -> std::optional<std::string_view> {
  // LowerBound supplies the only possible match; equality remains a raw byte
  // comparison and does not depend on host char signedness.
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
  // N separators describe N+1 children. Child zero is the dedicated leftmost
  // header link; every later child belongs to the separator on its left.
  TINYDB_CHECK(child_index <= separator_count_, "internal child index out of range");
  return child_index == 0 ? first_child_ : RightChildAt(child_index - 1);
}

auto InternalPageView::FindChildIndex(std::string_view key) const -> std::size_t {
  // This is upper_bound over encoded separators. Advancing on equality is the
  // B+ tree's "equal goes right" rule: a leaf split copies the first key of its
  // right half into the parent.
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
