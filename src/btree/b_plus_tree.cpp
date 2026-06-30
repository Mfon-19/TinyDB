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

BPlusTree::BPlusTree(BufferPool *buffer_pool, page_id_t root_page_id)
    : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

auto BPlusTree::Get(std::string_view key) -> std::optional<std::string> {
  // Take a copy of the root_page_id_, we'll modify this variable
  // in this function
  auto page_id = root_page_id_;

  while (true) {
    // Fetch the page, reinterpret_cast as the common NodeHeader
    // so we can check node type
    char *page = buffer_pool_->FetchPage(page_id);
    auto *node_header = reinterpret_cast<NodeHeader *>(page);

    if (node_header->type == NodeType::Leaf) {
      // We found the target leaf. Binary-search the sorted slot array to find
      // the first key greater than or equal to the requested key.
      auto *header = reinterpret_cast<LeafHeader *>(page);
      auto *slots =
          reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));

      std::uint16_t low = 0;
      std::uint16_t high = header->cell_count;
      while (low < high) {
        const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
        char *cell = page + slots[mid];
        auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
        // Cell key is just right after the header
        char *cell_key = cell + sizeof(LeafCellHeader);
        std::string_view cell_key_view(cell_key, cell_header->key_size);

        if (cell_key_view < key) {
          low = static_cast<std::uint16_t>(mid + 1);
        } else {
          high = mid;
        }
      }

      if (low < header->cell_count) {
        // A matching tombstoned cell is treated as absent.
        char *cell = page + slots[low];
        auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
        char *cell_key = cell + sizeof(LeafCellHeader);
        std::string_view cell_key_view(cell_key, cell_header->key_size);

        if (cell_key_view == key && cell_header->flags == 0) {
          char *value = cell_key + cell_header->key_size;
          auto result = std::string(value, cell_header->value_size);
          buffer_pool_->UnpinPage(page_id, false);
          return result;
        }
      }

      buffer_pool_->UnpinPage(page_id, false);
      return std::nullopt;
    }

    // Internal cells store separator keys and the child pointer to their right.
    // The first child pointer lives in the internal header.
    auto *header = reinterpret_cast<InternalHeader *>(page);
    auto *slots =
        reinterpret_cast<std::uint16_t *>(page + sizeof(InternalHeader));

    std::uint16_t low = 0;
    std::uint16_t high = header->cell_count;
    while (low < high) {
      const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
      char *cell = page + slots[mid];
      auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
      char *cell_key = cell + sizeof(InternalCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (key < cell_key_view) {
        high = mid;
      } else {
        low = static_cast<std::uint16_t>(mid + 1);
      }
    }

    page_id_t child_page_id = header->first_child;
    if (low > 0) {
      char *cell = page + slots[low - 1];
      auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
      child_page_id = cell_header->right_child;
    }

    buffer_pool_->UnpinPage(page_id, false);
    page_id = child_page_id;
  }
}

void BPlusTree::Put(std::string_view key, std::string_view value) {
  struct LeafRecord {
    std::string key;
    std::string value;
  };

  struct InternalRecord {
    std::string key;
    page_id_t right_child;
  };

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
      auto *header = reinterpret_cast<InternalHeader *>(page);
      auto *slots =
          reinterpret_cast<std::uint16_t *>(page + sizeof(InternalHeader));

      std::uint16_t low = 0;
      std::uint16_t high = header->cell_count;
      while (low < high) {
        const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
        char *cell = page + slots[mid];
        auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
        char *cell_key = cell + sizeof(InternalCellHeader);
        std::string_view cell_key_view(cell_key, cell_header->key_size);

        if (key < cell_key_view) {
          high = mid;
        } else {
          low = static_cast<std::uint16_t>(mid + 1);
        }
      }

      page_id_t child_page_id = header->first_child;
      if (low > 0) {
        char *cell = page + slots[low - 1];
        auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
        child_page_id = cell_header->right_child;
      }

      // Keep only the page id in the path. When a split propagates back up, we
      // binary-search the parent again instead of trying to preserve an index.
      parent_path.push_back(page_id);
      buffer_pool_->UnpinPage(page_id, false);
      page_id = child_page_id;
      continue;
    }

    // Binary-search the leaf slots
    auto *header = reinterpret_cast<LeafHeader *>(page);
    auto *slots = reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));

    std::uint16_t index = 0;
    std::uint16_t high = header->cell_count;
    while (index < high) {
      const auto mid = static_cast<std::uint16_t>(index + (high - index) / 2);
      char *cell = page + slots[mid];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (cell_key_view < key) {
        index = static_cast<std::uint16_t>(mid + 1);
      } else {
        high = mid;
      }
    }

    bool found = false;
    if (index < header->cell_count) {
      char *cell = page + slots[index];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);
      found = cell_key_view == key;
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
      char *cell = page + new_cell_offset;
      auto cell_header = LeafCellHeader{
          .key_size = static_cast<std::uint16_t>(key.size()),
          .value_size = static_cast<std::uint16_t>(value.size()),
          .flags = 0,
      };

      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::memcpy(cell + sizeof(LeafCellHeader), key.data(), key.size());
      std::memcpy(cell + sizeof(LeafCellHeader) + key.size(), value.data(),
                  value.size());

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
    // as ordinary records, apply this Put, sort them, then split that record
    // list into two leaves. Tombstoned cells are ignored during the rebuild.
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

    if (records.size() < 2) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf page full");
    }

    for (const auto &record : records) {
      const auto record_cell_size =
          sizeof(LeafCellHeader) + record.key.size() + record.value.size();
      auto cell_offset = std::uint16_t{0};
      auto record_fits = false;
      if (record_cell_size <= PAGE_SIZE) {
        cell_offset = static_cast<std::uint16_t>(
            (PAGE_SIZE - record_cell_size) &
            ~std::size_t{alignof(LeafCellHeader) - 1});
        record_fits = sizeof(LeafHeader) + sizeof(std::uint16_t) <= cell_offset;
      }

      if (!record_fits) {
        buffer_pool_->UnpinPage(page_id, false);
        throw std::runtime_error("leaf record too large");
      }
    }

    // Records are variable-sized, so split selection must prove both resulting
    // leaf pages can actually pack their cells. For every possible split, run
    // the same free_start/free_end math the writer uses below and keep the
    // valid split with the closest byte balance.
    auto split_index = std::size_t{0};
    auto found_leaf_split = false;
    auto best_leaf_imbalance = std::size_t{PAGE_SIZE * 2};
    for (std::size_t candidate = 1; candidate < records.size(); ++candidate) {
      auto left_free_start = std::size_t{sizeof(LeafHeader)};
      auto left_free_end = std::size_t{PAGE_SIZE};
      auto left_fits = true;
      for (std::size_t i = 0; i < candidate; ++i) {
        const auto left_candidate_cell_size = sizeof(LeafCellHeader) +
                                              records[i].key.size() +
                                              records[i].value.size();
        if (left_candidate_cell_size > left_free_end) {
          left_fits = false;
          break;
        }

        const auto cell_offset = (left_free_end - left_candidate_cell_size) &
                                 ~std::size_t{alignof(LeafCellHeader) - 1};
        left_free_start += sizeof(std::uint16_t);
        if (left_free_start > cell_offset) {
          left_fits = false;
          break;
        }
        left_free_end = cell_offset;
      }

      auto right_free_start = std::size_t{sizeof(LeafHeader)};
      auto right_free_end = std::size_t{PAGE_SIZE};
      auto right_fits = true;
      for (std::size_t i = candidate; i < records.size(); ++i) {
        const auto right_candidate_cell_size = sizeof(LeafCellHeader) +
                                               records[i].key.size() +
                                               records[i].value.size();
        if (right_candidate_cell_size > right_free_end) {
          right_fits = false;
          break;
        }

        const auto cell_offset = (right_free_end - right_candidate_cell_size) &
                                 ~std::size_t{alignof(LeafCellHeader) - 1};
        right_free_start += sizeof(std::uint16_t);
        if (right_free_start > cell_offset) {
          right_fits = false;
          break;
        }
        right_free_end = cell_offset;
      }

      if (!left_fits || !right_fits) {
        continue;
      }

      const auto left_used = left_free_start + PAGE_SIZE - left_free_end;
      const auto right_used = right_free_start + PAGE_SIZE - right_free_end;
      const auto imbalance = left_used > right_used ? left_used - right_used
                                                    : right_used - left_used;
      if (!found_leaf_split || imbalance < best_leaf_imbalance) {
        found_leaf_split = true;
        best_leaf_imbalance = imbalance;
        split_index = candidate;
      }
    }

    if (!found_leaf_split) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf split cannot fit records");
    }

    const auto leaf_separator_key = records[split_index].key;
    const auto separator_cell_size =
        sizeof(InternalCellHeader) + leaf_separator_key.size();
    auto separator_cell_offset = std::uint16_t{0};
    auto separator_fits_internal = false;
    if (separator_cell_size <= PAGE_SIZE) {
      separator_cell_offset = static_cast<std::uint16_t>(
          (PAGE_SIZE - separator_cell_size) &
          ~std::size_t{alignof(InternalCellHeader) - 1});
      separator_fits_internal =
          sizeof(InternalHeader) + sizeof(std::uint16_t) <=
          separator_cell_offset;
    }

    if (!separator_fits_internal) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf separator too large");
    }

    if (parent_path.empty()) {
      // Splitting the root leaf is special because root_page_id_ should keep
      // pointing at the root. Allocate two leaf children and rewrite the old
      // root page as an internal node that points at those children.
      page_id_t left_page_id = 0;
      char *left_page = buffer_pool_->NewPage(&left_page_id);
      auto *left_header = reinterpret_cast<LeafHeader *>(left_page);
      *left_header = LeafHeader{
          .type = NodeType::Leaf,
          .cell_count = 0,
          .free_start = sizeof(LeafHeader),
          .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
          .next_leaf = 0,
      };
      auto *left_slots =
          reinterpret_cast<std::uint16_t *>(left_page + sizeof(LeafHeader));

      page_id_t right_page_id = 0;
      char *right_page = buffer_pool_->NewPage(&right_page_id);
      auto *right_header = reinterpret_cast<LeafHeader *>(right_page);
      *right_header = LeafHeader{
          .type = NodeType::Leaf,
          .cell_count = 0,
          .free_start = sizeof(LeafHeader),
          .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
          .next_leaf = header->next_leaf,
      };
      auto *right_slots =
          reinterpret_cast<std::uint16_t *>(right_page + sizeof(LeafHeader));
      left_header->next_leaf = right_page_id;

      for (std::size_t i = 0; i < split_index; ++i) {
        const auto left_cell_size = sizeof(LeafCellHeader) +
                                    records[i].key.size() +
                                    records[i].value.size();
        const auto left_cell_offset = static_cast<std::uint16_t>(
            (left_header->free_end - left_cell_size) &
            ~std::size_t{alignof(LeafCellHeader) - 1});
        char *cell = left_page + left_cell_offset;
        auto cell_header = LeafCellHeader{
            .key_size = static_cast<std::uint16_t>(records[i].key.size()),
            .value_size = static_cast<std::uint16_t>(records[i].value.size()),
            .flags = 0,
        };

        std::memcpy(cell, &cell_header, sizeof(cell_header));
        std::copy(records[i].key.begin(), records[i].key.end(),
                  cell + sizeof(LeafCellHeader));
        std::copy(records[i].value.begin(), records[i].value.end(),
                  cell + sizeof(LeafCellHeader) + records[i].key.size());

        left_slots[left_header->cell_count] = left_cell_offset;
        ++left_header->cell_count;
        left_header->free_start = static_cast<std::uint16_t>(
            left_header->free_start + sizeof(std::uint16_t));
        left_header->free_end = left_cell_offset;
      }

      for (std::size_t i = split_index; i < records.size(); ++i) {
        const auto right_cell_size = sizeof(LeafCellHeader) +
                                     records[i].key.size() +
                                     records[i].value.size();
        const auto right_cell_offset = static_cast<std::uint16_t>(
            (right_header->free_end - right_cell_size) &
            ~std::size_t{alignof(LeafCellHeader) - 1});
        char *cell = right_page + right_cell_offset;
        auto cell_header = LeafCellHeader{
            .key_size = static_cast<std::uint16_t>(records[i].key.size()),
            .value_size = static_cast<std::uint16_t>(records[i].value.size()),
            .flags = 0,
        };

        std::memcpy(cell, &cell_header, sizeof(cell_header));
        std::copy(records[i].key.begin(), records[i].key.end(),
                  cell + sizeof(LeafCellHeader));
        std::copy(records[i].value.begin(), records[i].value.end(),
                  cell + sizeof(LeafCellHeader) + records[i].key.size());

        right_slots[right_header->cell_count] = right_cell_offset;
        ++right_header->cell_count;
        right_header->free_start = static_cast<std::uint16_t>(
            right_header->free_start + sizeof(std::uint16_t));
        right_header->free_end = right_cell_offset;
      }

      std::memset(page, 0, PAGE_SIZE);
      auto *root_header = reinterpret_cast<InternalHeader *>(page);
      *root_header = InternalHeader{
          .type = NodeType::Internal,
          .cell_count = 1,
          .free_start = static_cast<std::uint16_t>(sizeof(InternalHeader) +
                                                   sizeof(std::uint16_t)),
          .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
          .first_child = left_page_id,
      };

      const auto internal_cell_size =
          sizeof(InternalCellHeader) + leaf_separator_key.size();
      const auto internal_cell_offset = static_cast<std::uint16_t>(
          (root_header->free_end - internal_cell_size) &
          ~std::size_t{alignof(InternalCellHeader) - 1});
      auto *root_slots =
          reinterpret_cast<std::uint16_t *>(page + sizeof(InternalHeader));
      root_slots[0] = internal_cell_offset;
      root_header->free_end = internal_cell_offset;

      char *internal_cell = page + internal_cell_offset;
      auto internal_cell_header = InternalCellHeader{
          .right_child = right_page_id,
          .key_size = static_cast<std::uint16_t>(leaf_separator_key.size()),
      };
      std::memcpy(internal_cell, &internal_cell_header,
                  sizeof(internal_cell_header));
      std::copy(leaf_separator_key.begin(), leaf_separator_key.end(),
                internal_cell + sizeof(InternalCellHeader));

      buffer_pool_->UnpinPage(left_page_id, true);
      buffer_pool_->UnpinPage(right_page_id, true);
      buffer_pool_->UnpinPage(page_id, true);
      return;
    }

    // Non-root leaf split: keep the old leaf page as the left sibling so the
    // parent still points at a valid left child. The new page becomes the right
    // sibling, and its first key is inserted into the parent.
    const auto old_next_leaf = header->next_leaf;
    page_id_t right_page_id = 0;
    char *right_page = buffer_pool_->NewPage(&right_page_id);

    std::memset(page, 0, PAGE_SIZE);
    header = reinterpret_cast<LeafHeader *>(page);
    *header = LeafHeader{
        .type = NodeType::Leaf,
        .cell_count = 0,
        .free_start = sizeof(LeafHeader),
        .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
        .next_leaf = right_page_id,
    };
    slots = reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));

    auto *right_header = reinterpret_cast<LeafHeader *>(right_page);
    *right_header = LeafHeader{
        .type = NodeType::Leaf,
        .cell_count = 0,
        .free_start = sizeof(LeafHeader),
        .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
        .next_leaf = old_next_leaf,
    };
    auto *right_slots =
        reinterpret_cast<std::uint16_t *>(right_page + sizeof(LeafHeader));

    for (std::size_t i = 0; i < split_index; ++i) {
      const auto left_cell_size = sizeof(LeafCellHeader) +
                                  records[i].key.size() +
                                  records[i].value.size();
      const auto left_cell_offset =
          static_cast<std::uint16_t>((header->free_end - left_cell_size) &
                                     ~std::size_t{alignof(LeafCellHeader) - 1});
      char *cell = page + left_cell_offset;
      auto cell_header = LeafCellHeader{
          .key_size = static_cast<std::uint16_t>(records[i].key.size()),
          .value_size = static_cast<std::uint16_t>(records[i].value.size()),
          .flags = 0,
      };

      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(records[i].key.begin(), records[i].key.end(),
                cell + sizeof(LeafCellHeader));
      std::copy(records[i].value.begin(), records[i].value.end(),
                cell + sizeof(LeafCellHeader) + records[i].key.size());

      slots[header->cell_count] = left_cell_offset;
      ++header->cell_count;
      header->free_start = static_cast<std::uint16_t>(header->free_start +
                                                      sizeof(std::uint16_t));
      header->free_end = left_cell_offset;
    }

    for (std::size_t i = split_index; i < records.size(); ++i) {
      const auto right_cell_size = sizeof(LeafCellHeader) +
                                   records[i].key.size() +
                                   records[i].value.size();
      const auto right_cell_offset = static_cast<std::uint16_t>(
          (right_header->free_end - right_cell_size) &
          ~std::size_t{alignof(LeafCellHeader) - 1});
      char *cell = right_page + right_cell_offset;
      auto cell_header = LeafCellHeader{
          .key_size = static_cast<std::uint16_t>(records[i].key.size()),
          .value_size = static_cast<std::uint16_t>(records[i].value.size()),
          .flags = 0,
      };

      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(records[i].key.begin(), records[i].key.end(),
                cell + sizeof(LeafCellHeader));
      std::copy(records[i].value.begin(), records[i].value.end(),
                cell + sizeof(LeafCellHeader) + records[i].key.size());

      right_slots[right_header->cell_count] = right_cell_offset;
      ++right_header->cell_count;
      right_header->free_start = static_cast<std::uint16_t>(
          right_header->free_start + sizeof(std::uint16_t));
      right_header->free_end = right_cell_offset;
    }

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
    auto *parent_slots =
        reinterpret_cast<std::uint16_t *>(parent_page + sizeof(InternalHeader));

    // Try the cheap path first: insert the promoted separator directly into
    // the parent if its slotted page still has room for one more internal cell.
    std::uint16_t index = 0;
    std::uint16_t high = parent_header->cell_count;
    while (index < high) {
      const auto mid = static_cast<std::uint16_t>(index + (high - index) / 2);
      char *cell = parent_page + parent_slots[mid];
      auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
      char *cell_key = cell + sizeof(InternalCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (cell_key_view < separator_key) {
        index = static_cast<std::uint16_t>(mid + 1);
      } else {
        high = mid;
      }
    }

    const auto internal_cell_size =
        sizeof(InternalCellHeader) + separator_key.size();
    auto internal_cell_offset = std::uint16_t{0};
    auto internal_has_room = false;
    if (internal_cell_size <= parent_header->free_end) {
      internal_cell_offset = static_cast<std::uint16_t>(
          (parent_header->free_end - internal_cell_size) &
          ~std::size_t{alignof(InternalCellHeader) - 1});
      internal_has_room = parent_header->free_start + sizeof(std::uint16_t) <=
                          internal_cell_offset;
    }

    if (internal_has_room) {
      char *cell = parent_page + internal_cell_offset;
      auto cell_header = InternalCellHeader{
          .right_child = right_child_page_id,
          .key_size = static_cast<std::uint16_t>(separator_key.size()),
      };

      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(separator_key.begin(), separator_key.end(),
                cell + sizeof(InternalCellHeader));

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
      char *cell_key = cell + sizeof(InternalCellHeader);
      internal_records.push_back(InternalRecord{
          .key = std::string(cell_key, cell_header->key_size),
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

    // Internal separators are also variable-sized. A count-middle split can
    // overfill one child when a large separator lands near the middle, so try
    // every promotable separator and only accept candidates whose left and
    // right partitions both pack into one internal page.
    auto internal_split_index = std::size_t{0};
    auto found_internal_split = false;
    auto best_internal_imbalance = std::size_t{PAGE_SIZE * 2};
    for (std::size_t candidate = 0; candidate < internal_records.size();
         ++candidate) {
      const auto promoted_cell_size =
          sizeof(InternalCellHeader) + internal_records[candidate].key.size();
      auto promoted_fits_parent = false;
      if (promoted_cell_size <= PAGE_SIZE) {
        const auto promoted_cell_offset =
            (PAGE_SIZE - promoted_cell_size) &
            ~std::size_t{alignof(InternalCellHeader) - 1};
        promoted_fits_parent = sizeof(InternalHeader) + sizeof(std::uint16_t) <=
                               promoted_cell_offset;
      }
      if (!promoted_fits_parent) {
        continue;
      }

      auto left_free_start = std::size_t{sizeof(InternalHeader)};
      auto left_free_end = std::size_t{PAGE_SIZE};
      auto left_fits = true;
      for (std::size_t i = 0; i < candidate; ++i) {
        const auto cell_size =
            sizeof(InternalCellHeader) + internal_records[i].key.size();
        if (cell_size > left_free_end) {
          left_fits = false;
          break;
        }

        const auto cell_offset = (left_free_end - cell_size) &
                                 ~std::size_t{alignof(InternalCellHeader) - 1};
        left_free_start += sizeof(std::uint16_t);
        if (left_free_start > cell_offset) {
          left_fits = false;
          break;
        }
        left_free_end = cell_offset;
      }

      auto right_free_start = std::size_t{sizeof(InternalHeader)};
      auto right_free_end = std::size_t{PAGE_SIZE};
      auto right_fits = true;
      for (std::size_t i = candidate + 1; i < internal_records.size(); ++i) {
        const auto cell_size =
            sizeof(InternalCellHeader) + internal_records[i].key.size();
        if (cell_size > right_free_end) {
          right_fits = false;
          break;
        }

        const auto cell_offset = (right_free_end - cell_size) &
                                 ~std::size_t{alignof(InternalCellHeader) - 1};
        right_free_start += sizeof(std::uint16_t);
        if (right_free_start > cell_offset) {
          right_fits = false;
          break;
        }
        right_free_end = cell_offset;
      }

      if (!left_fits || !right_fits) {
        continue;
      }

      const auto left_used = left_free_start + PAGE_SIZE - left_free_end;
      const auto right_used = right_free_start + PAGE_SIZE - right_free_end;
      const auto imbalance = left_used > right_used ? left_used - right_used
                                                    : right_used - left_used;
      if (!found_internal_split || imbalance < best_internal_imbalance) {
        found_internal_split = true;
        best_internal_imbalance = imbalance;
        internal_split_index = candidate;
      }
    }

    if (!found_internal_split) {
      buffer_pool_->UnpinPage(parent_page_id, false);
      throw std::runtime_error("internal split cannot fit records");
    }

    const auto promoted_key = internal_records[internal_split_index].key;
    const auto right_first_child =
        internal_records[internal_split_index].right_child;

    if (parent_path.empty()) {
      // The full parent is the root. Keep root_page_id_ stable by allocating
      // two internal children, then rewrite the old root page as their parent.
      page_id_t left_page_id = 0;
      char *left_page = buffer_pool_->NewPage(&left_page_id);
      auto *left_header = reinterpret_cast<InternalHeader *>(left_page);
      *left_header = InternalHeader{
          .type = NodeType::Internal,
          .cell_count = 0,
          .free_start = sizeof(InternalHeader),
          .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
          .first_child = old_first_child,
      };
      auto *left_slots =
          reinterpret_cast<std::uint16_t *>(left_page + sizeof(InternalHeader));

      for (std::size_t i = 0; i < internal_split_index; ++i) {
        const auto left_cell_size =
            sizeof(InternalCellHeader) + internal_records[i].key.size();
        const auto left_cell_offset = static_cast<std::uint16_t>(
            (left_header->free_end - left_cell_size) &
            ~std::size_t{alignof(InternalCellHeader) - 1});
        char *cell = left_page + left_cell_offset;
        auto cell_header = InternalCellHeader{
            .right_child = internal_records[i].right_child,
            .key_size =
                static_cast<std::uint16_t>(internal_records[i].key.size()),
        };

        std::memcpy(cell, &cell_header, sizeof(cell_header));
        std::copy(internal_records[i].key.begin(),
                  internal_records[i].key.end(),
                  cell + sizeof(InternalCellHeader));

        left_slots[left_header->cell_count] = left_cell_offset;
        ++left_header->cell_count;
        left_header->free_start = static_cast<std::uint16_t>(
            left_header->free_start + sizeof(std::uint16_t));
        left_header->free_end = left_cell_offset;
      }

      page_id_t right_page_id = 0;
      char *right_page = buffer_pool_->NewPage(&right_page_id);
      auto *right_header = reinterpret_cast<InternalHeader *>(right_page);
      *right_header = InternalHeader{
          .type = NodeType::Internal,
          .cell_count = 0,
          .free_start = sizeof(InternalHeader),
          .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
          .first_child = right_first_child,
      };
      auto *right_slots = reinterpret_cast<std::uint16_t *>(
          right_page + sizeof(InternalHeader));

      for (std::size_t i = internal_split_index + 1;
           i < internal_records.size(); ++i) {
        const auto right_cell_size =
            sizeof(InternalCellHeader) + internal_records[i].key.size();
        const auto right_cell_offset = static_cast<std::uint16_t>(
            (right_header->free_end - right_cell_size) &
            ~std::size_t{alignof(InternalCellHeader) - 1});
        char *cell = right_page + right_cell_offset;
        auto cell_header = InternalCellHeader{
            .right_child = internal_records[i].right_child,
            .key_size =
                static_cast<std::uint16_t>(internal_records[i].key.size()),
        };

        std::memcpy(cell, &cell_header, sizeof(cell_header));
        std::copy(internal_records[i].key.begin(),
                  internal_records[i].key.end(),
                  cell + sizeof(InternalCellHeader));

        right_slots[right_header->cell_count] = right_cell_offset;
        ++right_header->cell_count;
        right_header->free_start = static_cast<std::uint16_t>(
            right_header->free_start + sizeof(std::uint16_t));
        right_header->free_end = right_cell_offset;
      }

      std::memset(parent_page, 0, PAGE_SIZE);
      parent_header = reinterpret_cast<InternalHeader *>(parent_page);
      *parent_header = InternalHeader{
          .type = NodeType::Internal,
          .cell_count = 1,
          .free_start = static_cast<std::uint16_t>(sizeof(InternalHeader) +
                                                   sizeof(std::uint16_t)),
          .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
          .first_child = left_page_id,
      };
      parent_slots = reinterpret_cast<std::uint16_t *>(parent_page +
                                                       sizeof(InternalHeader));

      const auto root_cell_size =
          sizeof(InternalCellHeader) + promoted_key.size();
      const auto root_cell_offset = static_cast<std::uint16_t>(
          (parent_header->free_end - root_cell_size) &
          ~std::size_t{alignof(InternalCellHeader) - 1});
      parent_slots[0] = root_cell_offset;
      parent_header->free_end = root_cell_offset;

      char *cell = parent_page + root_cell_offset;
      auto cell_header = InternalCellHeader{
          .right_child = right_page_id,
          .key_size = static_cast<std::uint16_t>(promoted_key.size()),
      };
      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(promoted_key.begin(), promoted_key.end(),
                cell + sizeof(InternalCellHeader));

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

    std::memset(parent_page, 0, PAGE_SIZE);
    parent_header = reinterpret_cast<InternalHeader *>(parent_page);
    *parent_header = InternalHeader{
        .type = NodeType::Internal,
        .cell_count = 0,
        .free_start = sizeof(InternalHeader),
        .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
        .first_child = old_first_child,
    };
    parent_slots =
        reinterpret_cast<std::uint16_t *>(parent_page + sizeof(InternalHeader));

    for (std::size_t i = 0; i < internal_split_index; ++i) {
      const auto left_cell_size =
          sizeof(InternalCellHeader) + internal_records[i].key.size();
      const auto left_cell_offset = static_cast<std::uint16_t>(
          (parent_header->free_end - left_cell_size) &
          ~std::size_t{alignof(InternalCellHeader) - 1});
      char *cell = parent_page + left_cell_offset;
      auto cell_header = InternalCellHeader{
          .right_child = internal_records[i].right_child,
          .key_size =
              static_cast<std::uint16_t>(internal_records[i].key.size()),
      };

      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(internal_records[i].key.begin(), internal_records[i].key.end(),
                cell + sizeof(InternalCellHeader));

      parent_slots[parent_header->cell_count] = left_cell_offset;
      ++parent_header->cell_count;
      parent_header->free_start = static_cast<std::uint16_t>(
          parent_header->free_start + sizeof(std::uint16_t));
      parent_header->free_end = left_cell_offset;
    }

    auto *right_header = reinterpret_cast<InternalHeader *>(right_page);
    *right_header = InternalHeader{
        .type = NodeType::Internal,
        .cell_count = 0,
        .free_start = sizeof(InternalHeader),
        .free_end = static_cast<std::uint16_t>(PAGE_SIZE),
        .first_child = right_first_child,
    };
    auto *right_slots =
        reinterpret_cast<std::uint16_t *>(right_page + sizeof(InternalHeader));

    for (std::size_t i = internal_split_index + 1; i < internal_records.size();
         ++i) {
      const auto right_cell_size =
          sizeof(InternalCellHeader) + internal_records[i].key.size();
      const auto right_cell_offset = static_cast<std::uint16_t>(
          (right_header->free_end - right_cell_size) &
          ~std::size_t{alignof(InternalCellHeader) - 1});
      char *cell = right_page + right_cell_offset;
      auto cell_header = InternalCellHeader{
          .right_child = internal_records[i].right_child,
          .key_size =
              static_cast<std::uint16_t>(internal_records[i].key.size()),
      };

      std::memcpy(cell, &cell_header, sizeof(cell_header));
      std::copy(internal_records[i].key.begin(), internal_records[i].key.end(),
                cell + sizeof(InternalCellHeader));

      right_slots[right_header->cell_count] = right_cell_offset;
      ++right_header->cell_count;
      right_header->free_start = static_cast<std::uint16_t>(
          right_header->free_start + sizeof(std::uint16_t));
      right_header->free_end = right_cell_offset;
    }

    separator_key = promoted_key;
    right_child_page_id = right_page_id;

    buffer_pool_->UnpinPage(right_page_id, true);
    buffer_pool_->UnpinPage(parent_page_id, true);
  }
}

void BPlusTree::Remove(std::string_view key) {
  page_id_t page_id = root_page_id_;

  while (true) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *node_header = reinterpret_cast<NodeHeader *>(page);

    // Route through internal pages using the same separator-key search as Get.
    if (node_header->type == NodeType::Internal) {
      auto *header = reinterpret_cast<InternalHeader *>(page);
      auto *slots =
          reinterpret_cast<std::uint16_t *>(page + sizeof(InternalHeader));

      std::uint16_t low = 0;
      std::uint16_t high = header->cell_count;
      while (low < high) {
        const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
        char *cell = page + slots[mid];
        auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
        char *cell_key = cell + sizeof(InternalCellHeader);
        std::string_view cell_key_view(cell_key, cell_header->key_size);

        if (key < cell_key_view) {
          high = mid;
        } else {
          low = static_cast<std::uint16_t>(mid + 1);
        }
      }

      page_id_t child_page_id = header->first_child;
      if (low > 0) {
        char *cell = page + slots[low - 1];
        auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
        child_page_id = cell_header->right_child;
      }

      buffer_pool_->UnpinPage(page_id, false);
      page_id = child_page_id;
      continue;
    }

    // Deletion is logical for now: find the key in the leaf and mark its cell
    // as tombstoned. The slot stays in place until compaction exists.
    auto *header = reinterpret_cast<LeafHeader *>(page);
    auto *slots = reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));
    bool dirty = false;

    std::uint16_t low = 0;
    std::uint16_t high = header->cell_count;
    while (low < high) {
      const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
      char *cell = page + slots[mid];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (cell_key_view < key) {
        low = static_cast<std::uint16_t>(mid + 1);
      } else {
        high = mid;
      }
    }

    if (low < header->cell_count) {
      char *cell = page + slots[low];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (cell_key_view == key) {
        cell_header->flags = 1;
        dirty = true;
      }
    }

    buffer_pool_->UnpinPage(page_id, dirty);
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

    auto *header = reinterpret_cast<InternalHeader *>(page);
    auto *slots =
        reinterpret_cast<std::uint16_t *>(page + sizeof(InternalHeader));

    std::uint16_t low = 0;
    std::uint16_t high = header->cell_count;
    while (low < high) {
      const auto mid = static_cast<std::uint16_t>(low + (high - low) / 2);
      char *cell = page + slots[mid];
      auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
      char *cell_key = cell + sizeof(InternalCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (start < cell_key_view) {
        high = mid;
      } else {
        low = static_cast<std::uint16_t>(mid + 1);
      }
    }

    page_id_t child_page_id = header->first_child;
    if (low > 0) {
      char *cell = page + slots[low - 1];
      auto *cell_header = reinterpret_cast<InternalCellHeader *>(cell);
      child_page_id = cell_header->right_child;
    }

    buffer_pool_->UnpinPage(page_id, false);
    page_id = child_page_id;
  }

  // Then walk the leaf chain left-to-right, collecting live cells until end.
  while (page_id != HEADER_PAGE_ID) {
    char *page = buffer_pool_->FetchPage(page_id);
    auto *header = reinterpret_cast<LeafHeader *>(page);
    auto *slots = reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));

    // On each leaf, skip directly to the first slot whose key could be in the
    // requested range.
    std::uint16_t index = 0;
    std::uint16_t high = header->cell_count;
    while (index < high) {
      const auto mid = static_cast<std::uint16_t>(index + (high - index) / 2);
      char *cell = page + slots[mid];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

      if (cell_key_view < start) {
        index = static_cast<std::uint16_t>(mid + 1);
      } else {
        high = mid;
      }
    }

    bool done = false;
    for (std::uint16_t i = index; i < header->cell_count; ++i) {
      char *cell = page + slots[i];
      auto *cell_header = reinterpret_cast<LeafCellHeader *>(cell);
      char *cell_key = cell + sizeof(LeafCellHeader);
      std::string_view cell_key_view(cell_key, cell_header->key_size);

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
