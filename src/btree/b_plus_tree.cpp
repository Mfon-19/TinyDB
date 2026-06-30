#include <tinydb/b_plus_tree.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

struct LeafRecord {
  std::string key;
  std::string value;
};

struct InternalRecord {
  std::string key;
  page_id_t right_child;
};

static constexpr auto LEAF_MIN_BYTES = PAGE_SIZE / 2;

// The slot array starts immediately after the fixed-size leaf header.
static auto LeafSlots(char *page) -> std::uint16_t * {
  return reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));
}

// Internal pages use the same slotted layout, but start after InternalHeader.
static auto InternalSlots(char *page) -> std::uint16_t * {
  return reinterpret_cast<std::uint16_t *>(page + sizeof(InternalHeader));
}

// Leaf cell bytes are: LeafCellHeader, key bytes, then value bytes.
static auto LeafCellKey(char *cell) -> std::string_view {
  auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
  char *cell_key = cell + sizeof(LeafCellHeader);
  return {cell_key, cell_header->key_size};
}

// Internal cell bytes are: InternalCellHeader, then separator key bytes.
static auto InternalCellKey(char *cell) -> std::string_view {
  auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
  char *cell_key = cell + sizeof(InternalCellHeader);
  return {cell_key, cell_header->key_size};
}

// Slotted pages pack cells from the end of the page downward. This computes the
// next aligned cell offset and verifies that the slot array still has room.
static auto TryCellOffset(std::size_t free_start, std::size_t free_end,
                          std::size_t cell_size, std::size_t slot_size,
                          std::size_t alignment,
                          std::uint16_t *cell_offset) -> bool {
  if (cell_size > free_end) {
    return false;
  }

  const auto offset = (free_end - cell_size) & ~(alignment - 1);
  if (free_start + slot_size > offset) {
    return false;
  }

  *cell_offset = static_cast<std::uint16_t>(offset);
  return true;
}

// Lower-bound search on a leaf: first slot whose key is >= key.
static auto FindLeafSlot(char *page, std::string_view key) -> std::uint16_t {
  auto *header = reinterpret_cast<LeafHeader *>(page);
  auto *slots = LeafSlots(page);

  std::uint16_t low = 0;
  std::uint16_t high = header->cell_count;
  while (low < high) {
    const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
    char *cell = page + slots[mid];

    if (LeafCellKey(cell) < key) {
      low = static_cast<std::uint16_t>(mid + 1);
    } else {
      high = mid;
    }
  }

  return low;
}

// Lower-bound search for inserting a new separator into an internal page.
static auto FindInternalInsertSlot(char *page,
                                   std::string_view key) -> std::uint16_t {
  auto *header = reinterpret_cast<InternalHeader *>(page);
  auto *slots = InternalSlots(page);

  std::uint16_t low = 0;
  std::uint16_t high = header->cell_count;
  while (low < high) {
    const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
    char *cell = page + slots[mid];

    if (InternalCellKey(cell) < key) {
      low = static_cast<std::uint16_t>(mid + 1);
    } else {
      high = mid;
    }
  }

  return low;
}

// Internal page routing uses upper-bound semantics:
//   key < K0        -> first_child
//   K0 <= key < K1  -> slot 0 right_child
//   key >= last key -> last slot right_child
static auto FindInternalChild(char *page, std::string_view key) -> page_id_t {
  auto *header = reinterpret_cast<InternalHeader *>(page);
  auto *slots = InternalSlots(page);

  std::uint16_t low = 0;
  std::uint16_t high = header->cell_count;
  while (low < high) {
    const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
    char *cell = page + slots[mid];

    if (key < InternalCellKey(cell)) {
      high = mid;
    } else {
      low = static_cast<std::uint16_t>(mid + 1);
    }
  }

  if (low == 0) {
    return header->first_child;
  }

  char *cell = page + slots[low - 1];
  auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
  return cell_header->right_child;
}

// Serialize a leaf cell at an already validated offset.
static void WriteLeafCell(char *page, std::uint16_t offset,
                          std::string_view key, std::string_view value) {
  char *cell = page + offset;
  auto cell_header = LeafCellHeader{
      .key_size = static_cast<std::uint16_t>(key.size()),
      .value_size = static_cast<std::uint16_t>(value.size()),
      .flags = 0,
  };

  std::memcpy(cell, &cell_header, sizeof(cell_header));
  std::copy(key.begin(), key.end(), cell + sizeof(LeafCellHeader));
  std::copy(value.begin(), value.end(),
            cell + sizeof(LeafCellHeader) + key.size());
}

// Serialize an internal separator cell at an already validated offset.
static void WriteInternalCell(char *page, std::uint16_t offset,
                              std::string_view key, page_id_t right_child) {
  char *cell = page + offset;
  auto cell_header = InternalCellHeader{
      .right_child = right_child,
      .key_size = static_cast<std::uint16_t>(key.size()),
  };

  std::memcpy(cell, &cell_header, sizeof(cell_header));
  std::copy(key.begin(), key.end(), cell + sizeof(InternalCellHeader));
}

// Read live leaf cells into ordinary C++ records. Tombstoned cells stay on the
// page until some later rewrite, but they do not participate in searches,
// compaction, splitting, or stealing.
static auto ReadLeafRecords(char *page) -> std::vector<LeafRecord> {
  auto *header = reinterpret_cast<LeafHeader *>(page);
  auto *slots = LeafSlots(page);
  auto records = std::vector<LeafRecord>{};
  records.reserve(header->cell_count);

  for (std::uint16_t i = 0; i < header->cell_count; ++i) {
    char *cell = page + slots[i];
    auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
    if (cell_header->flags == 1) {
      continue;
    }

    char *cell_key = cell + sizeof(LeafCellHeader);
    char *cell_value = cell_key + cell_header->key_size;
    records.push_back(LeafRecord{
        .key = std::string(cell_key, cell_header->key_size),
        .value = std::string(cell_value, cell_header->value_size),
    });
  }

  return records;
}

// Read internal separator cells into ordinary records while keeping the
// separate first_child pointer in the page header.
static auto ReadInternalRecords(char *page) -> std::vector<InternalRecord> {
  auto *header = reinterpret_cast<InternalHeader *>(page);
  auto *slots = InternalSlots(page);
  auto records = std::vector<InternalRecord>{};
  records.reserve(header->cell_count);

  for (std::uint16_t i = 0; i < header->cell_count; ++i) {
    char *cell = page + slots[i];
    auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
    records.push_back(InternalRecord{
        .key = std::string(InternalCellKey(cell)),
        .right_child = cell_header->right_child,
    });
  }

  return records;
}

// Return child[index] from an internal node. child[0] is first_child; every
// later child is stored as the right_child of the previous separator cell.
static auto InternalChildAt(char *page, std::size_t index) -> page_id_t {
  auto *header = reinterpret_cast<InternalHeader *>(page);
  auto *slots = InternalSlots(page);

  if (index > header->cell_count) {
    throw std::runtime_error("internal child index out of range");
  }

  if (index == 0) {
    return header->first_child;
  }

  char *cell = page + slots[index - 1];
  auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
  return cell_header->right_child;
}

// Find the position of a child pointer inside its parent. This is used by leaf
// stealing to locate adjacent siblings and the separator that must change.
static auto FindInternalChildIndex(char *page,
                                   page_id_t child_page_id) -> std::size_t {
  auto *header = reinterpret_cast<InternalHeader *>(page);

  for (std::size_t index = 0; index <= header->cell_count; ++index) {
    if (InternalChildAt(page, index) == child_page_id) {
      return index;
    }
  }

  throw std::runtime_error("child page not found in parent");
}

// Simulate packing a range of leaf records. Returning nullopt means the range
// cannot fit in one leaf page using the real page layout.
static auto PackedLeafBytes(const std::vector<LeafRecord> &records,
                            std::size_t begin,
                            std::size_t end) -> std::optional<std::size_t> {
  auto free_start = std::size_t{sizeof(LeafHeader)};
  auto free_end = std::size_t{PAGE_SIZE};

  for (std::size_t i = begin; i < end; ++i) {
    const auto cell_size = sizeof(LeafCellHeader) + records[i].key.size() +
                           records[i].value.size();
    auto cell_offset = std::uint16_t{0};
    if (!TryCellOffset(free_start, free_end, cell_size, sizeof(std::uint16_t),
                       alignof(LeafCellHeader), &cell_offset)) {
      return std::nullopt;
    }

    free_start += sizeof(std::uint16_t);
    free_end = cell_offset;
  }

  return free_start + PAGE_SIZE - free_end;
}

// Simulate packing a range of internal records. The first_child pointer is in
// the header, so only separator cells need slots/cell bytes.
static auto PackedInternalBytes(const std::vector<InternalRecord> &records,
                                std::size_t begin,
                                std::size_t end) -> std::optional<std::size_t> {
  auto free_start = std::size_t{sizeof(InternalHeader)};
  auto free_end = std::size_t{PAGE_SIZE};

  for (std::size_t i = begin; i < end; ++i) {
    const auto cell_size = sizeof(InternalCellHeader) + records[i].key.size();
    auto cell_offset = std::uint16_t{0};
    if (!TryCellOffset(free_start, free_end, cell_size, sizeof(std::uint16_t),
                       alignof(InternalCellHeader), &cell_offset)) {
      return std::nullopt;
    }

    free_start += sizeof(std::uint16_t);
    free_end = cell_offset;
  }

  return free_start + PAGE_SIZE - free_end;
}

// Choose a leaf split where both sides fit. Among valid splits, prefer the one
// closest to equal byte usage, not equal record count.
static auto ChooseLeafSplit(const std::vector<LeafRecord> &records)
    -> std::optional<std::size_t> {
  auto split_index = std::size_t{0};
  auto found_split = false;
  auto best_imbalance = std::size_t{PAGE_SIZE * 2};

  for (std::size_t candidate = 1; candidate < records.size(); ++candidate) {
    const auto left_used = PackedLeafBytes(records, 0, candidate);
    const auto right_used = PackedLeafBytes(records, candidate, records.size());
    if (!left_used.has_value() || !right_used.has_value()) {
      continue;
    }

    const auto imbalance = *left_used > *right_used ? *left_used - *right_used
                                                    : *right_used - *left_used;
    if (!found_split || imbalance < best_imbalance) {
      found_split = true;
      best_imbalance = imbalance;
      split_index = candidate;
    }
  }

  if (!found_split) {
    return std::nullopt;
  }
  return split_index;
}

// Choose a redistribution point for two adjacent leaves after a delete. Unlike
// split selection, this enforces the minimum-fill rule for both resulting
// leaves. If no candidate satisfies that, merge is the next policy, but merge
// is intentionally not implemented yet.
static auto ChooseLeafRedistributionSplit(
    const std::vector<LeafRecord> &records) -> std::optional<std::size_t> {
  auto split_index = std::size_t{0};
  auto found_split = false;
  auto best_imbalance = std::size_t{PAGE_SIZE * 2};

  for (std::size_t candidate = 1; candidate < records.size(); ++candidate) {
    const auto left_used = PackedLeafBytes(records, 0, candidate);
    const auto right_used = PackedLeafBytes(records, candidate, records.size());
    if (!left_used.has_value() || !right_used.has_value()) {
      continue;
    }

    if (*left_used < LEAF_MIN_BYTES || *right_used < LEAF_MIN_BYTES) {
      continue;
    }

    const auto imbalance = *left_used > *right_used ? *left_used - *right_used
                                                    : *right_used - *left_used;
    if (!found_split || imbalance < best_imbalance) {
      found_split = true;
      best_imbalance = imbalance;
      split_index = candidate;
    }
  }

  if (!found_split) {
    return std::nullopt;
  }
  return split_index;
}

// A promoted separator must be able to live as a single cell in some parent.
static auto InternalSeparatorFits(std::string_view key) -> bool {
  auto cell_offset = std::uint16_t{0};
  return TryCellOffset(sizeof(InternalHeader), PAGE_SIZE,
                       sizeof(InternalCellHeader) + key.size(),
                       sizeof(std::uint16_t), alignof(InternalCellHeader),
                       &cell_offset);
}

// Choose the internal separator to promote. The promoted record is excluded
// from both children; its right_child becomes the right child's first_child.
static auto ChooseInternalSplit(const std::vector<InternalRecord> &records)
    -> std::optional<std::size_t> {
  auto split_index = std::size_t{0};
  auto found_split = false;
  auto best_imbalance = std::size_t{PAGE_SIZE * 2};

  for (std::size_t candidate = 0; candidate < records.size(); ++candidate) {
    if (!InternalSeparatorFits(records[candidate].key)) {
      continue;
    }

    const auto left_used = PackedInternalBytes(records, 0, candidate);
    const auto right_used =
        PackedInternalBytes(records, candidate + 1, records.size());
    if (!left_used.has_value() || !right_used.has_value()) {
      continue;
    }

    const auto imbalance = *left_used > *right_used ? *left_used - *right_used
                                                    : *right_used - *left_used;
    if (!found_split || imbalance < best_imbalance) {
      found_split = true;
      best_imbalance = imbalance;
      split_index = candidate;
    }
  }

  if (!found_split) {
    return std::nullopt;
  }
  return split_index;
}

// Rewrite a whole leaf page from sorted records. Used for root and non-root
// splits so the packing code stays identical in both cases.
static void PackLeafPage(char *page, const std::vector<LeafRecord> &records,
                         std::size_t begin, std::size_t end,
                         page_id_t next_leaf) {
  std::memset(page, 0, PAGE_SIZE);
  auto *header = reinterpret_cast<LeafHeader *>(page);
  *header = LeafHeader{
      .type = NodeType::Leaf,
      .cell_count = 0,
      .free_start = static_cast<std::uint16_t>(sizeof(LeafHeader)),
      .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
      .next_leaf = next_leaf,
  };
  auto *slots = LeafSlots(page);

  for (std::size_t i = begin; i < end; ++i) {
    const auto cell_size = sizeof(LeafCellHeader) + records[i].key.size() +
                           records[i].value.size();
    auto cell_offset = std::uint16_t{0};
    if (!TryCellOffset(header->free_start, header->free_end, cell_size,
                       sizeof(std::uint16_t), alignof(LeafCellHeader),
                       &cell_offset)) {
      throw std::runtime_error("leaf page overflow");
    }

    WriteLeafCell(page, cell_offset, records[i].key, records[i].value);
    slots[header->cell_count] = cell_offset;
    ++header->cell_count;
    header->free_start =
        static_cast<std::uint16_t>(header->free_start + sizeof(std::uint16_t));
    header->free_end = cell_offset;
  }
}

// Rewrite a whole internal page from sorted separator records. The first child
// is kept separately because the internal cell stores only the right child.
static void PackInternalPage(char *page, page_id_t first_child,
                             const std::vector<InternalRecord> &records,
                             std::size_t begin, std::size_t end) {
  std::memset(page, 0, PAGE_SIZE);
  auto *header = reinterpret_cast<InternalHeader *>(page);
  *header = InternalHeader{
      .type = NodeType::Internal,
      .cell_count = 0,
      .free_start = static_cast<std::uint16_t>(sizeof(InternalHeader)),
      .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
      .first_child = first_child,
  };
  auto *slots = InternalSlots(page);

  for (std::size_t i = begin; i < end; ++i) {
    const auto cell_size = sizeof(InternalCellHeader) + records[i].key.size();
    auto cell_offset = std::uint16_t{0};
    if (!TryCellOffset(header->free_start, header->free_end, cell_size,
                       sizeof(std::uint16_t), alignof(InternalCellHeader),
                       &cell_offset)) {
      throw std::runtime_error("internal page overflow");
    }

    WriteInternalCell(page, cell_offset, records[i].key,
                      records[i].right_child);
    slots[header->cell_count] = cell_offset;
    ++header->cell_count;
    header->free_start =
        static_cast<std::uint16_t>(header->free_start + sizeof(std::uint16_t));
    header->free_end = cell_offset;
  }
}

// Rewriting an internal separator may change its byte size, so update the
// parent by rebuilding the whole internal page. If the replacement separator
// would not fit, leave the parent untouched and let a later merge policy handle
// the underfull leaf.
static auto TryUpdateInternalSeparator(char *page, std::size_t separator_index,
                                       std::string_view key) -> bool {
  auto *header = reinterpret_cast<InternalHeader *>(page);
  const auto first_child = header->first_child;
  auto records = ReadInternalRecords(page);

  if (separator_index >= records.size()) {
    throw std::runtime_error("internal separator index out of range");
  }

  records[separator_index].key = std::string(key);
  if (!PackedInternalBytes(records, 0, records.size()).has_value()) {
    return false;
  }

  PackInternalPage(page, first_child, records, 0, records.size());
  return true;
}

BPlusTree::BPlusTree(BufferPool *buffer_pool, page_id_t root_page_id)
    : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

auto BPlusTree::Get(std::string_view key) -> std::optional<std::string> {
  auto page_id = root_page_id_;

  while (true) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *node_header = reinterpret_cast<NodeHeader *>(page);

    if (node_header->type == NodeType::Leaf) {
      auto *header = reinterpret_cast<LeafHeader *>(page);
      auto *slots = LeafSlots(page);
      const auto index = FindLeafSlot(page, key);

      if (index < header->cell_count) {
        // A matching tombstoned cell is treated as absent.
        char *cell = page + slots[index];
        auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
        char *cell_key = cell + sizeof(LeafCellHeader);

        if (LeafCellKey(cell) == key && cell_header->flags == 0) {
          char *value = cell_key + cell_header->key_size;
          auto result = std::string(value, cell_header->value_size);
          buffer_pool_->UnpinPage(page_id, false);
          return result;
        }
      }

      buffer_pool_->UnpinPage(page_id, false);
      return std::nullopt;
    }

    const auto child_page_id = FindInternalChild(page, key);
    buffer_pool_->UnpinPage(page_id, false);
    page_id = child_page_id;
  }
}

void BPlusTree::Put(std::string_view key, std::string_view value) {
  // We descend from root to leaf, but splits move upward from leaf to root.
  // This vector is used as a stack: push while descending, pop while splitting.
  auto parent_path = std::vector<page_id_t>{};
  page_id_t page_id = root_page_id_;
  auto separator_key = std::string{};
  page_id_t right_child_page_id = HEADER_PAGE_ID;

  while (true) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *node_header = reinterpret_cast<NodeHeader *>(page);

    // Walk internal pages until we reach the leaf that should contain key.
    if (node_header->type == NodeType::Internal) {
      // Keep only the page id in the path. When a split propagates back up, we
      // binary-search the parent again instead of trying to preserve an index.
      const auto child_page_id = FindInternalChild(page, key);
      parent_path.push_back(page_id);
      buffer_pool_->UnpinPage(page_id, false);
      page_id = child_page_id;
      continue;
    }

    // Binary-search the leaf slots
    auto *header = reinterpret_cast<LeafHeader *>(page);
    auto *slots = LeafSlots(page);
    const auto index = FindLeafSlot(page, key);

    bool found = false;
    if (index < header->cell_count) {
      char *cell = page + slots[index];
      found = LeafCellKey(cell) == key;
    }

    const auto cell_size = sizeof(LeafCellHeader) + key.size() + value.size();
    const auto slot_size = found ? 0 : sizeof(std::uint16_t);
    auto new_cell_offset = std::uint16_t{0};
    auto leaf_has_room = false;
    if (cell_size <= header->free_end) {
      new_cell_offset =
          static_cast<std::uint16_t>((header->free_end - cell_size) &
                                     ~std::size_t{alignof(LeafCellHeader) - 1});
      leaf_has_room = header->free_start + slot_size <= new_cell_offset;
    }

    // If the leaf has room, write the new cell bytes and point the sorted slot
    // at them. Replacing a key leaves the old cell unreachable for now.
    if (leaf_has_room) {
      WriteLeafCell(page, new_cell_offset, key, value);

      if (!found) {
        for (std::uint16_t i = header->cell_count; i > index; --i) {
          slots[i] = slots[i - 1];
        }
        ++header->cell_count;
        header->free_start = static_cast<std::uint16_t>(header->free_start +
                                                        sizeof(std::uint16_t));
      }

      slots[index] = new_cell_offset;
      header->free_end = new_cell_offset;
      buffer_pool_->UnpinPage(page_id, true);
      return;
    }

    // The leaf has no physical space for the new cell. Rebuild the live cells
    // as ordinary records and apply this Put in memory. Tombstoned cells are
    // ignored, which is the opportunistic compaction step.
    auto records = std::vector<LeafRecord>{};
    bool replaced = false;
    for (std::uint16_t i = 0; i < header->cell_count; ++i) {
      char *cell = page + slots[i];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      if (cell_header->flags == 1) {
        continue;
      }

      char *cell_key = cell + sizeof(LeafCellHeader);
      char *cell_value = cell_key + cell_header->key_size;
      auto record_key = std::string(cell_key, cell_header->key_size);
      auto record_value = std::string(cell_value, cell_header->value_size);

      if (record_key == key) {
        record_value = std::string(value);
        replaced = true;
      }

      records.push_back(LeafRecord{.key = record_key, .value = record_value});
    }

    if (!replaced) {
      records.push_back(
          LeafRecord{.key = std::string(key), .value = std::string(value)});
    }

    std::sort(records.begin(), records.end(),
              [](const LeafRecord &left, const LeafRecord &right) {
                return left.key < right.key;
              });

    // Compact before splitting. If the live records plus this Put fit in the
    // same leaf, rewrite that leaf without tombstones and avoid touching the
    // parent.
    const auto old_next_leaf = header->next_leaf;
    if (PackedLeafBytes(records, 0, records.size()).has_value()) {
      PackLeafPage(page, records, 0, records.size(), old_next_leaf);
      buffer_pool_->UnpinPage(page_id, true);
      return;
    }

    if (records.size() < 2) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf page full");
    }

    const auto split_index = ChooseLeafSplit(records);
    if (!split_index.has_value()) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf split cannot fit records");
    }

    const auto leaf_separator_key = records[*split_index].key;
    if (!InternalSeparatorFits(leaf_separator_key)) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf separator too large");
    }

    if (parent_path.empty()) {
      // Splitting the root leaf is special because root_page_id_ should keep
      // pointing at the root. Allocate two leaf children and rewrite the old
      // root page as an internal node that points at those children.
      page_id_t left_page_id = 0;
      char *left_page = buffer_pool_->NewPage(&left_page_id);
      page_id_t right_page_id = 0;
      char *right_page = buffer_pool_->NewPage(&right_page_id);
      PackLeafPage(left_page, records, 0, *split_index, right_page_id);
      PackLeafPage(right_page, records, *split_index, records.size(),
                   old_next_leaf);

      const auto root_records = std::vector<InternalRecord>{
          InternalRecord{.key = leaf_separator_key,
                         .right_child = right_page_id},
      };
      PackInternalPage(page, left_page_id, root_records, 0,
                       root_records.size());

      buffer_pool_->UnpinPage(left_page_id, true);
      buffer_pool_->UnpinPage(right_page_id, true);
      buffer_pool_->UnpinPage(page_id, true);
      return;
    }

    // Non-root leaf split: keep the old leaf page as the left sibling so the
    // parent still points at a valid left child. The new page becomes the right
    // sibling, and its first key is inserted into the parent.
    page_id_t right_page_id = 0;
    char *right_page = buffer_pool_->NewPage(&right_page_id);
    PackLeafPage(page, records, 0, *split_index, right_page_id);
    PackLeafPage(right_page, records, *split_index, records.size(),
                 old_next_leaf);

    separator_key = leaf_separator_key;
    right_child_page_id = right_page_id;

    buffer_pool_->UnpinPage(right_page_id, true);
    buffer_pool_->UnpinPage(page_id, true);
    break;
  }

  while (true) {
    const auto parent_page_id = parent_path.back();
    parent_path.pop_back();

    char *parent_page = buffer_pool_->FetchPage(parent_page_id);
    auto *parent_header = reinterpret_cast<InternalHeader *>(parent_page);
    auto *parent_slots = InternalSlots(parent_page);

    // Try the cheap path first: insert the promoted separator directly into
    // the parent if its slotted page still has room for one more internal cell.
    const auto index = FindInternalInsertSlot(parent_page, separator_key);
    const auto internal_cell_size =
        sizeof(InternalCellHeader) + separator_key.size();
    auto internal_cell_offset = std::uint16_t{0};
    if (TryCellOffset(parent_header->free_start, parent_header->free_end,
                      internal_cell_size, sizeof(std::uint16_t),
                      alignof(InternalCellHeader), &internal_cell_offset)) {
      WriteInternalCell(parent_page, internal_cell_offset, separator_key,
                        right_child_page_id);
      for (std::uint16_t i = parent_header->cell_count; i > index; --i) {
        parent_slots[i] = parent_slots[i - 1];
      }

      parent_slots[index] = internal_cell_offset;
      ++parent_header->cell_count;
      parent_header->free_start = static_cast<std::uint16_t>(
          parent_header->free_start + sizeof(std::uint16_t));
      parent_header->free_end = internal_cell_offset;

      buffer_pool_->UnpinPage(parent_page_id, true);
      return;
    }

    // Parent is full. Rebuild its separator cells plus the promoted separator
    // from below, split the sorted list, and promote the middle separator up.
    const auto old_first_child = parent_header->first_child;
    auto internal_records = std::vector<InternalRecord>{};
    for (std::uint16_t i = 0; i < parent_header->cell_count; ++i) {
      char *cell = parent_page + parent_slots[i];
      auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
      internal_records.push_back(InternalRecord{
          .key = std::string(InternalCellKey(cell)),
          .right_child = cell_header->right_child,
      });
    }

    internal_records.push_back(InternalRecord{
        .key = separator_key,
        .right_child = right_child_page_id,
    });

    std::sort(internal_records.begin(), internal_records.end(),
              [](const InternalRecord &left, const InternalRecord &right) {
                return left.key < right.key;
              });

    const auto internal_split_index = ChooseInternalSplit(internal_records);
    if (!internal_split_index.has_value()) {
      buffer_pool_->UnpinPage(parent_page_id, false);
      throw std::runtime_error("internal split cannot fit records");
    }

    const auto promoted_key = internal_records[*internal_split_index].key;
    const auto right_first_child =
        internal_records[*internal_split_index].right_child;

    if (parent_path.empty()) {
      // The full parent is the root. Keep root_page_id_ stable by allocating
      // two internal children, then rewrite the old root page as their parent.
      page_id_t left_page_id = 0;
      char *left_page = buffer_pool_->NewPage(&left_page_id);
      page_id_t right_page_id = 0;
      char *right_page = buffer_pool_->NewPage(&right_page_id);
      PackInternalPage(left_page, old_first_child, internal_records, 0,
                       *internal_split_index);
      PackInternalPage(right_page, right_first_child, internal_records,
                       *internal_split_index + 1, internal_records.size());

      const auto root_records = std::vector<InternalRecord>{
          InternalRecord{.key = promoted_key, .right_child = right_page_id},
      };
      PackInternalPage(parent_page, left_page_id, root_records, 0,
                       root_records.size());

      buffer_pool_->UnpinPage(left_page_id, true);
      buffer_pool_->UnpinPage(right_page_id, true);
      buffer_pool_->UnpinPage(parent_page_id, true);
      return;
    }

    // Non-root internal split: keep the old page as the left sibling and
    // allocate one right sibling. The middle separator is removed from both
    // siblings and propagated to the next parent up the stack.
    page_id_t right_page_id = 0;
    char *right_page = buffer_pool_->NewPage(&right_page_id);
    PackInternalPage(parent_page, old_first_child, internal_records, 0,
                     *internal_split_index);
    PackInternalPage(right_page, right_first_child, internal_records,
                     *internal_split_index + 1, internal_records.size());

    separator_key = promoted_key;
    right_child_page_id = right_page_id;

    buffer_pool_->UnpinPage(right_page_id, true);
    buffer_pool_->UnpinPage(parent_page_id, true);
  }
}

void BPlusTree::Remove(std::string_view key) {
  // We need the immediate parent if the leaf underflows, so keep the page ids
  // seen on the descent. Ancestors are left alone for now because this step
  // only redistributes leaf records; merge/internal repair comes later.
  auto parent_path = std::vector<page_id_t>{};
  page_id_t page_id = root_page_id_;

  while (true) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *node_header = reinterpret_cast<NodeHeader *>(page);

    // Route through internal pages using the same separator-key search as Get.
    if (node_header->type == NodeType::Internal) {
      const auto child_page_id = FindInternalChild(page, key);
      parent_path.push_back(page_id);
      buffer_pool_->UnpinPage(page_id, false);
      page_id = child_page_id;
      continue;
    }

    // Deletion is logical for now: find the key in the leaf and mark its cell
    // as tombstoned. If the leaf falls below the minimum byte occupancy, try to
    // steal records from an adjacent leaf with the same parent.
    auto *header = reinterpret_cast<LeafHeader *>(page);
    auto *slots = LeafSlots(page);
    const auto index = FindLeafSlot(page, key);

    if (index >= header->cell_count) {
      buffer_pool_->UnpinPage(page_id, false);
      return;
    }

    char *cell = page + slots[index];
    auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
    if (LeafCellKey(cell) != key || cell_header->flags == 1) {
      buffer_pool_->UnpinPage(page_id, false);
      return;
    }

    cell_header->flags = 1;

    auto records = ReadLeafRecords(page);
    const auto used_bytes = PackedLeafBytes(records, 0, records.size());
    if (!used_bytes.has_value()) {
      buffer_pool_->UnpinPage(page_id, true);
      throw std::runtime_error("leaf page cannot pack live records");
    }

    // Root leaves are allowed to be small. Non-root leaves that remain at least
    // half full also need no structural repair, so the tombstone can stay until
    // an insert needs the space.
    if (parent_path.empty() || *used_bytes >= LEAF_MIN_BYTES) {
      buffer_pool_->UnpinPage(page_id, true);
      return;
    }

    const auto target_next_leaf = header->next_leaf;
    const auto parent_page_id = parent_path.back();
    char *parent_page = buffer_pool_->FetchPage(parent_page_id);
    auto *parent_header = reinterpret_cast<InternalHeader *>(parent_page);
    const auto child_index = FindInternalChildIndex(parent_page, page_id);

    // Prefer the left sibling. It keeps the target's right-link unchanged and
    // moves the largest left records into the front of the underfull target.
    if (child_index > 0) {
      const auto left_page_id = InternalChildAt(parent_page, child_index - 1);
      char *left_page = buffer_pool_->FetchPage(left_page_id);
      auto *left_node_header = reinterpret_cast<NodeHeader *>(left_page);

      if (left_node_header->type != NodeType::Leaf) {
        buffer_pool_->UnpinPage(left_page_id, false);
        buffer_pool_->UnpinPage(parent_page_id, false);
        buffer_pool_->UnpinPage(page_id, true);
        throw std::runtime_error("leaf sibling is not a leaf");
      }

      auto combined_records = ReadLeafRecords(left_page);
      combined_records.insert(combined_records.end(), records.begin(),
                              records.end());

      const auto split_index = ChooseLeafRedistributionSplit(combined_records);
      if (split_index.has_value() &&
          TryUpdateInternalSeparator(parent_page, child_index - 1,
                                     combined_records[*split_index].key)) {
        PackLeafPage(left_page, combined_records, 0, *split_index, page_id);
        PackLeafPage(page, combined_records, *split_index,
                     combined_records.size(), target_next_leaf);

        buffer_pool_->UnpinPage(left_page_id, true);
        buffer_pool_->UnpinPage(parent_page_id, true);
        buffer_pool_->UnpinPage(page_id, true);
        return;
      }

      buffer_pool_->UnpinPage(left_page_id, false);
    }

    // If the left sibling cannot donate, try the right sibling. The target
    // keeps the low side of the combined key range, and the parent's separator
    // for the right child becomes the right leaf's new first key.
    if (child_index < parent_header->cell_count) {
      const auto right_page_id = InternalChildAt(parent_page, child_index + 1);
      char *right_page = buffer_pool_->FetchPage(right_page_id);
      auto *right_node_header = reinterpret_cast<NodeHeader *>(right_page);

      if (right_node_header->type != NodeType::Leaf) {
        buffer_pool_->UnpinPage(right_page_id, false);
        buffer_pool_->UnpinPage(parent_page_id, false);
        buffer_pool_->UnpinPage(page_id, true);
        throw std::runtime_error("leaf sibling is not a leaf");
      }

      auto *right_header = reinterpret_cast<LeafHeader *>(right_page);
      const auto right_next_leaf = right_header->next_leaf;
      auto combined_records = records;
      const auto right_records = ReadLeafRecords(right_page);
      combined_records.insert(combined_records.end(), right_records.begin(),
                              right_records.end());

      const auto split_index = ChooseLeafRedistributionSplit(combined_records);
      if (split_index.has_value() &&
          TryUpdateInternalSeparator(parent_page, child_index,
                                     combined_records[*split_index].key)) {
        PackLeafPage(page, combined_records, 0, *split_index, right_page_id);
        PackLeafPage(right_page, combined_records, *split_index,
                     combined_records.size(), right_next_leaf);

        buffer_pool_->UnpinPage(right_page_id, true);
        buffer_pool_->UnpinPage(parent_page_id, true);
        buffer_pool_->UnpinPage(page_id, true);
        return;
      }

      buffer_pool_->UnpinPage(right_page_id, false);
    }

    // No sibling can donate while preserving the minimum-fill rule. Leave the
    // leaf underfull for the future merge implementation.
    buffer_pool_->UnpinPage(parent_page_id, false);
    buffer_pool_->UnpinPage(page_id, true);
    return;
  }
}

auto BPlusTree::Scan(std::string_view start, std::string_view end)
    -> std::vector<std::pair<std::string, std::string>> {
  auto results = std::vector<std::pair<std::string, std::string>>{};
  auto page_id = root_page_id_;

  // First route to the leaf that may contain the start key.
  while (true) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *node_header = reinterpret_cast<NodeHeader *>(page);

    if (node_header->type == NodeType::Leaf) {
      buffer_pool_->UnpinPage(page_id, false);
      break;
    }

    const auto child_page_id = FindInternalChild(page, start);
    buffer_pool_->UnpinPage(page_id, false);
    page_id = child_page_id;
  }

  // Then walk the leaf chain left-to-right, collecting live cells until end.
  while (page_id != HEADER_PAGE_ID) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *header = reinterpret_cast<LeafHeader *>(page);
    auto *slots = LeafSlots(page);
    const auto index = FindLeafSlot(page, start);

    bool done = false;
    for (std::uint16_t i = index; i < header->cell_count; ++i) {
      char *cell = page + slots[i];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      const auto cell_key_view = LeafCellKey(cell);

      if (cell_key_view >= end) {
        done = true;
        break;
      }

      if (cell_header->flags == 0) {
        char *value = cell_key + cell_header->key_size;
        results.emplace_back(std::string(cell_key, cell_header->key_size),
                             std::string(value, cell_header->value_size));
      }
    }

    const auto next_leaf = header->next_leaf;
    buffer_pool_->UnpinPage(page_id, false);

    if (done) {
      break;
    }
    page_id = next_leaf;
  }

  return results;
}

}  // namespace tinydb
