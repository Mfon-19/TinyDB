#include "btree/page_format.h"

#include "storage/encoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tinydb {
namespace {

auto Bytes(const char *page) -> std::span<const std::byte> { return std::as_bytes(std::span{page, PAGE_SIZE}); }

auto ValidateSlots(std::span<const std::byte> page, storage::DataPageType type, std::uint16_t cell_count,
                   std::uint16_t free_start, std::uint16_t free_end) -> Status {
  const auto header_bytes = type == storage::DataPageType::Leaf ? LEAF_HEADER_SIZE : INTERNAL_HEADER_SIZE;
  const auto expected_free_start = header_bytes + static_cast<std::size_t>(cell_count) * SLOT_SIZE;
  if (free_start != expected_free_start || free_start > free_end || free_end > PAGE_SIZE) {
    return Status::Corruption("invalid tree-page free-space bounds");
  }

  auto previous_key = std::string_view{};
  bool first = true;
  for (std::size_t i = 0; i < cell_count; ++i) {
    const auto slot = storage::GetLittleEndian<slot_t>(page, header_bytes + i * SLOT_SIZE);
    if (!slot || *slot < free_end || *slot >= PAGE_SIZE) {
      return Status::Corruption("tree-page slot points outside the cell region");
    }

    std::size_t key_offset = 0;
    std::size_t key_bytes = 0;
    std::size_t cell_bytes = 0;
    if (type == storage::DataPageType::Leaf) {
      const auto key = storage::GetLittleEndian<std::uint16_t>(page, *slot + leaf_cell_offset::KEY_BYTES);
      const auto value = storage::GetLittleEndian<std::uint16_t>(page, *slot + leaf_cell_offset::VALUE_BYTES);
      if (!key || !value || *slot + LEAF_CELL_HEADER_SIZE > PAGE_SIZE ||
          page[*slot + leaf_cell_offset::FLAGS] != std::byte{0}) {
        return Status::Corruption("invalid leaf-cell header");
      }
      key_offset = *slot + LEAF_CELL_HEADER_SIZE;
      key_bytes = *key;
      cell_bytes = LEAF_CELL_HEADER_SIZE + *key + *value;
    } else {
      const auto child = storage::GetLittleEndian<page_id_t>(page, *slot + internal_cell_offset::RIGHT_CHILD);
      const auto key = storage::GetLittleEndian<std::uint16_t>(page, *slot + internal_cell_offset::KEY_BYTES);
      if (!child || !key || *child < FIRST_DATA_PAGE_ID || *slot + INTERNAL_CELL_HEADER_SIZE > PAGE_SIZE) {
        return Status::Corruption("invalid internal-cell header");
      }
      key_offset = *slot + INTERNAL_CELL_HEADER_SIZE;
      key_bytes = *key;
      cell_bytes = INTERNAL_CELL_HEADER_SIZE + *key;
    }
    if (cell_bytes > PAGE_SIZE - *slot) {
      return Status::Corruption("tree-page cell overruns the page");
    }
    const auto key = std::string_view{reinterpret_cast<const char *>(page.data() + key_offset), key_bytes};
    if (!first && !(previous_key < key)) {
      return Status::Corruption("tree-page keys are not strictly ordered");
    }
    previous_key = key;
    first = false;
  }
  return {};
}

}  // namespace

auto RawNodeType(const char *page) -> std::uint16_t {
  return storage::GetLittleEndian<std::uint16_t>(Bytes(page), storage::data_page_offset::TYPE).value_or(0);
}

auto ValidateTreePage(const char *page, page_id_t expected_page_id) -> Status {
  const auto bytes = Bytes(page);
  const auto common = storage::DecodeDataPageHeader(bytes, expected_page_id);
  if (!common) {
    return common.error();
  }
  if (common->type != storage::DataPageType::Leaf && common->type != storage::DataPageType::Internal) {
    return Status::Corruption("page is not a B+ tree node");
  }
  if (common->payload_bytes != PAGE_SIZE - storage::data_page_offset::HEADER_BYTES) {
    return Status::Corruption("tree page does not cover the complete page payload");
  }

  const auto cell_count = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::CELL_COUNT);
  const auto free_start = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::FREE_START);
  const auto free_end = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::FREE_END);
  const auto reserved = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::RESERVED);
  const auto link = storage::GetLittleEndian<page_id_t>(bytes, node_page_offset::LINK);
  if (!cell_count || !free_start || !free_end || !reserved || !link || *reserved != 0) {
    return Status::Corruption("truncated or invalid tree-page header");
  }
  if (common->type == storage::DataPageType::Internal && *link < FIRST_DATA_PAGE_ID) {
    return Status::Corruption("internal page has an invalid first child");
  }
  if (common->type == storage::DataPageType::Leaf && *link != HEADER_PAGE_ID && *link < FIRST_DATA_PAGE_ID) {
    return Status::Corruption("leaf page has an invalid successor");
  }
  return ValidateSlots(bytes, common->type, *cell_count, *free_start, *free_end);
}

}  // namespace tinydb
