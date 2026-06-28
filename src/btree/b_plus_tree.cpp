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
  auto page_id = root_page_id_;

  while (true) {
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
  page_id_t page_id = root_page_id_;

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

      buffer_pool_->UnpinPage(page_id, false);
      page_id = child_page_id;
      continue;
    }

    // Binary-search the leaf slots. The slots are sorted by key even though
    // the cell bytes themselves are packed from the end of the page downward.
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
    const auto new_cell_offset =
        static_cast<std::uint16_t>((header->free_end - cell_size) &
                                   ~std::size_t{alignof(LeafCellHeader) - 1});

    // If the leaf has room, write the new cell bytes and point the sorted slot
    // at them. Replacing a key leaves the old cell unreachable for now.
    if (header->free_start + slot_size <= new_cell_offset) {
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

    // This milestone only knows how to split the initial root leaf. Splitting
    // a child leaf and inserting into an existing internal parent comes next.
    if (page_id != root_page_id_) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf page full");
    }

    struct Record {
      std::string key;
      std::string value;
    };

    // Rebuild the full root leaf as records, apply this Put, then split that
    // sorted record list into two new leaves.
    auto records = std::vector<Record>{};
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

      records.push_back(Record{.key = record_key, .value = record_value});
    }

    if (!replaced) {
      records.push_back(
          Record{.key = std::string(key), .value = std::string(value)});
    }

    std::sort(records.begin(), records.end(),
              [](const Record &left, const Record &right) {
                return left.key < right.key;
              });

    if (records.size() < 2) {
      buffer_pool_->UnpinPage(page_id, false);
      throw std::runtime_error("leaf page full");
    }

    const auto split_index = records.size() / 2;
    const auto separator_key = records[split_index].key;

    // Pack the lower half into a new left leaf.
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

    for (std::size_t i = 0; i < split_index; ++i) {
      const auto left_cell_size = sizeof(LeafCellHeader) +
                                  records[i].key.size() +
                                  records[i].value.size();
      const auto left_cell_offset =
          static_cast<std::uint16_t>((left_header->free_end - left_cell_size) &
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

    // Pack the upper half into a new right leaf and link the two leaves for
    // ordered scans.
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
    left_header->next_leaf = right_page_id;

    auto *right_slots =
        reinterpret_cast<std::uint16_t *>(right_page + sizeof(LeafHeader));
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

    // Reuse the original root page as the new internal root. Its first child is
    // the left leaf, and its first separator routes greater/equal keys right.
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
        sizeof(InternalCellHeader) + separator_key.size();
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
        .key_size = static_cast<std::uint16_t>(separator_key.size()),
    };
    std::memcpy(internal_cell, &internal_cell_header,
                sizeof(internal_cell_header));
    std::copy(separator_key.begin(), separator_key.end(),
              internal_cell + sizeof(InternalCellHeader));

    buffer_pool_->UnpinPage(left_page_id, true);
    buffer_pool_->UnpinPage(right_page_id, true);
    buffer_pool_->UnpinPage(page_id, true);
    return;
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
