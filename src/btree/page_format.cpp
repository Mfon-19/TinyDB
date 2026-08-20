#include "btree/page_format.h"

#include "btree/page_source.h"
#include "storage/encoding.h"
#include "txn/contract.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tinydb {
namespace {

auto PageSpan(const char *page) -> std::span<const std::byte, PAGE_SIZE> {
  return std::as_bytes(std::span<const char, PAGE_SIZE>{page, PAGE_SIZE});
}

/*
** Validate each slot and the cell it names. In addition to local bounds, this
** checks the canonical free_start value, strict key order, and the encoded
** fields that PageView later reads without further validation.
*/
auto ValidateSlots(std::span<const std::byte> page, storage::DataPageType type, std::uint16_t cell_count,
                   std::uint16_t free_start, std::uint16_t free_end) -> Status {
  // Builders emit one canonical slot boundary. Exact equality catches a cell
  // count that disagrees with the encoded slot region.
  const auto header_bytes = type == storage::DataPageType::Leaf ? LEAF_HEADER_SIZE : INTERNAL_HEADER_SIZE;
  const auto expected_free_start = header_bytes + static_cast<std::size_t>(cell_count) * SLOT_SIZE;
  if (free_start != expected_free_start || free_start > free_end || free_end > PAGE_SIZE) {
    return Status::Corruption("invalid tree-page free-space bounds");
  }

  auto previous_key = std::string_view{};
  bool first = true;
  for (std::size_t i = 0; i < cell_count; ++i) {
    const auto slot = storage::GetLittleEndianUnchecked<slot_t>(page, header_bytes + i * SLOT_SIZE);
    if (slot < free_end || slot >= PAGE_SIZE) {
      return Status::Corruption("tree-page slot points outside the cell region");
    }

    std::size_t key_offset = 0;
    std::size_t key_bytes = 0;
    std::size_t cell_bytes = 0;
    if (type == storage::DataPageType::Leaf) {
      if (slot > PAGE_SIZE - LEAF_CELL_HEADER_SIZE) {
        return Status::Corruption("invalid leaf-cell header");
      }
      const auto key = storage::GetLittleEndianUnchecked<std::uint16_t>(page, slot + leaf_cell_offset::KEY_BYTES);
      const auto value = storage::GetLittleEndianUnchecked<std::uint16_t>(page, slot + leaf_cell_offset::VALUE_BYTES);
      const auto kind = std::to_integer<std::uint8_t>(page[slot + leaf_cell_offset::VALUE_KIND]);
      if (kind != static_cast<std::uint8_t>(LeafValueKind::Inline) &&
          kind != static_cast<std::uint8_t>(LeafValueKind::Overflow)) {
        return Status::Corruption("leaf cell has an unknown value kind");
      }
      key_offset = slot + LEAF_CELL_HEADER_SIZE;
      key_bytes = key;
      cell_bytes = LEAF_CELL_HEADER_SIZE + key + value;
      if (kind == static_cast<std::uint8_t>(LeafValueKind::Overflow)) {
        if (value != OVERFLOW_VALUE_DESCRIPTOR_BYTES || cell_bytes > PAGE_SIZE - slot) {
          return Status::Corruption("overflow leaf cell has an invalid descriptor length");
        }
        const auto descriptor_offset = key_offset + key_bytes;
        const auto total = storage::GetLittleEndianUnchecked<std::uint64_t>(
            page, descriptor_offset + overflow_descriptor_offset::TOTAL_VALUE_BYTES);
        const auto first_page = storage::GetLittleEndianUnchecked<page_id_t>(
            page, descriptor_offset + overflow_descriptor_offset::FIRST_PAGE_ID);
        if (total == 0 || total > MAX_VALUE_BYTES || first_page < FIRST_DATA_PAGE_ID) {
          return Status::Corruption("leaf cell contains an invalid overflow descriptor");
        }
      }
    } else {
      if (slot > PAGE_SIZE - INTERNAL_CELL_HEADER_SIZE) {
        return Status::Corruption("invalid internal-cell header");
      }
      const auto child = storage::GetLittleEndianUnchecked<page_id_t>(page, slot + internal_cell_offset::RIGHT_CHILD);
      const auto key = storage::GetLittleEndianUnchecked<std::uint16_t>(page, slot + internal_cell_offset::KEY_BYTES);
      if (child < FIRST_DATA_PAGE_ID) {
        return Status::Corruption("invalid internal-cell header");
      }
      key_offset = slot + INTERNAL_CELL_HEADER_SIZE;
      key_bytes = key;
      cell_bytes = INTERNAL_CELL_HEADER_SIZE + key;
    }
    if (cell_bytes > PAGE_SIZE - slot) {
      return Status::Corruption("tree-page cell overruns the page");
    }
    const auto key = std::string_view{reinterpret_cast<const char *>(page.data() + key_offset), key_bytes};
    if (!first && !txn::BytewiseLess{}(previous_key, key)) {
      return Status::Corruption("tree-page keys are not strictly ordered");
    }
    previous_key = key;
    first = false;
  }
  return {};
}

auto ValidateTreePayload(const char *page, const storage::DataPageHeader &common) -> Status {
  const auto bytes = PageSpan(page);
  if (common.type != storage::DataPageType::Leaf && common.type != storage::DataPageType::Internal) {
    return Status::Corruption("page is not a B+ tree node");
  }
  if (common.payload_bytes != PAGE_SIZE - storage::data_page_offset::HEADER_BYTES) {
    return Status::Corruption("tree page does not cover the complete page payload");
  }

  const auto cell_count = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, node_page_offset::CELL_COUNT);
  const auto free_start = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, node_page_offset::FREE_START);
  const auto free_end = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, node_page_offset::FREE_END);
  const auto reserved = storage::GetLittleEndianUnchecked<std::uint16_t>(bytes, node_page_offset::RESERVED);
  const auto link = storage::GetLittleEndianUnchecked<page_id_t>(bytes, node_page_offset::LINK);
  if (reserved != 0) {
    return Status::Corruption("invalid tree-page reserved field");
  }
  if (common.type == storage::DataPageType::Internal && link < FIRST_DATA_PAGE_ID) {
    return Status::Corruption("internal page has an invalid first child");
  }
  if (common.type == storage::DataPageType::Leaf && link != HEADER_PAGE_ID && link < FIRST_DATA_PAGE_ID) {
    return Status::Corruption("leaf page has an invalid successor");
  }
  return ValidateSlots(bytes, common.type, cell_count, free_start, free_end);
}

}  // namespace
auto RawNodeType(const char *page) -> std::uint16_t {
  return storage::GetLittleEndianUnchecked<std::uint16_t>(PageSpan(page), storage::data_page_offset::TYPE);
}

auto ValidateTreePage(const char *page, page_id_t expected_page_id) -> Status {
  const auto common = storage::DecodeDataPageHeader(PageSpan(page), expected_page_id);
  if (!common) {
    return common.error();
  }
  return ValidateTreePayload(page, *common);
}

auto ValidateTreePagePayload(const char *page, const storage::DataPageHeader &validated_header) -> Status {
  return ValidateTreePayload(page, validated_header);
}

}  // namespace tinydb
