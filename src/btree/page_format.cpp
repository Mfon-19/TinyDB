#include "btree/page_format.h"

#include "storage/encoding.h"
#include "txn/contract.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tinydb {
namespace {

/*
** Validation observes persistent bytes and never normalizes them in place.
** Once this routine succeeds, page views rely on its proof and use internal
** checks only to detect an impossible mutation of supposedly immutable bytes.
*/
auto Bytes(const char *page) -> std::span<const std::byte> { return std::as_bytes(std::span{page, PAGE_SIZE}); }

/*
** Validate the complete slot/cell region for one node. In addition to local
** bounds, this proves canonical free_start, strict key order, legal child
** references, and non-overlapping interpretation of the cell payloads.
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
    // A slot may reference only the descending cell region.
    const auto slot = storage::GetLittleEndian<slot_t>(page, header_bytes + i * SLOT_SIZE);
    if (!slot || *slot < free_end || *slot >= PAGE_SIZE) {
      return Status::Corruption("tree-page slot points outside the cell region");
    }

    std::size_t key_offset = 0;
    std::size_t key_bytes = 0;
    std::size_t cell_bytes = 0;
    if (type == storage::DataPageType::Leaf) {
      // Prove lengths and the reserved byte before constructing borrowed views.
      const auto key = storage::GetLittleEndian<std::uint16_t>(page, *slot + leaf_cell_offset::KEY_BYTES);
      const auto value = storage::GetLittleEndian<std::uint16_t>(page, *slot + leaf_cell_offset::VALUE_BYTES);
      if (!key || !value || *slot + LEAF_CELL_HEADER_SIZE > PAGE_SIZE ||
          page[*slot + leaf_cell_offset::RESERVED] != std::byte{0}) {
        return Status::Corruption("invalid leaf-cell header");
      }
      key_offset = *slot + LEAF_CELL_HEADER_SIZE;
      key_bytes = *key;
      cell_bytes = LEAF_CELL_HEADER_SIZE + *key + *value;
    } else {
      // Every internal record owns a real right child.
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
    // Strict byte ordering rejects duplicate keys and ambiguous separators.
    if (!first && !txn::BytewiseLess{}(previous_key, key)) {
      return Status::Corruption("tree-page keys are not strictly ordered");
    }
    previous_key = key;
    first = false;
  }
  return {};
}

}  // namespace

auto RawNodeType(const char *page) -> std::uint16_t {
  // Zero is reserved for fresh-root detection and is never a valid page type.
  return storage::GetLittleEndian<std::uint16_t>(Bytes(page), storage::data_page_offset::TYPE).value_or(0);
}

auto ValidateTreePage(const char *page, page_id_t expected_page_id) -> Status {
  // Common framing and checksum validation always precede tree-local offsets.
  const auto bytes = Bytes(page);
  const auto common = storage::DecodeDataPageHeader(bytes, expected_page_id);
  if (!common) {
    return common.error();
  }
  if (common->type != storage::DataPageType::Leaf && common->type != storage::DataPageType::Internal) {
    return Status::Corruption("page is not a B+ tree node");
  }
  if (common->payload_bytes != PAGE_SIZE - storage::data_page_offset::HEADER_BYTES) {
    // Tree pages own the complete payload, including their internal free space.
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
    // Internal LINK is a mandatory leftmost child.
    return Status::Corruption("internal page has an invalid first child");
  }
  if (common->type == storage::DataPageType::Leaf && *link != HEADER_PAGE_ID && *link < FIRST_DATA_PAGE_ID) {
    // Leaf LINK may be zero only as the chain terminator.
    return Status::Corruption("leaf page has an invalid successor");
  }
  return ValidateSlots(bytes, common->type, *cell_count, *free_start, *free_end);
}

}  // namespace tinydb
