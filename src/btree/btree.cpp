#include <tinydb/btree.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

BTree::BTree(BufferPool *buffer_pool, page_id_t root_page_id)
    : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

auto BTree::Get(std::string_view key) -> std::optional<std::string> {
  // Raw bytes here, the whole 4096
  auto *root_page = buffer_pool_->FetchPage(root_page_id_);

  // Interpret the first few bytes: root_page[0...sizeof(LeafHeader)]
  // as the leaf header
  auto *header = reinterpret_cast<LeafHeader *>(root_page);

  // After the leaf header, the rest of the bytes are the slots. Each slot
  // is an offset to a cell
  // slots[0] = 4060, slots[1] = 4020, ...
  // a cell at offset slots[i] is parsed as:
  // uint16_t key_size, uint16_t value_size, uint8_t flags, key bytes, value
  // bytes
  auto *slots =
      reinterpret_cast<std::uint16_t *>(root_page + sizeof(LeafHeader));

  // Do a linear search for now to find the key
  for (std::uint16_t i = 0; i < header->cell_count; i++) {
    char *cell = root_page + slots[i];
    auto *cell_header = reinterpret_cast<CellHeader *>(cell);
    char *cell_key = cell + sizeof(CellHeader);
    std::string_view cell_key_view(cell_key, cell_header->key_size);

    if (cell_key_view == key) {
      buffer_pool_->UnpinPage(root_page_id_, false);

      // Flag is set if cell is tombstoned
      if (cell_header->flags == 1) {
        return std::nullopt;
      }

      char *value = cell_key + cell_header->key_size;
      return std::string(value, cell_header->value_size);
    }

    if (cell_key_view > key) {
      break;
    }
  }

  buffer_pool_->UnpinPage(root_page_id_, false);
  return std::nullopt;
}

void BTree::Put(std::string_view key, std::string_view value) {
  auto *page = buffer_pool_->FetchPage(root_page_id_);
  auto *header = reinterpret_cast<LeafHeader *>(page);
  auto *slots = reinterpret_cast<uint16_t *>(page + sizeof(LeafHeader));

  // Find sorted position for new slot in slot array.
  std::uint16_t index = header->cell_count;
  bool found = false;
  for (std::uint16_t i = 0; i < header->cell_count; i++) {
    char *cell = page + slots[i];
    auto *cell_header = reinterpret_cast<CellHeader *>(cell);
    char *cell_key = cell + sizeof(CellHeader);
    std::string_view cell_key_view(cell_key, cell_header->key_size);

    if (cell_key_view == key) {
      index = i;
      found = true;
      break;
    }

    if (cell_key_view > key) {
      index = i;
      break;
    }
  }

  // A new key and value + its spot in the slot array must fit in between
  // free_start and free_end, if not, throw an exception. for now, we just use
  // one page.
  const auto cell_size = sizeof(CellHeader) + key.size() + value.size();
  const auto slot_size = found ? 0 : sizeof(std::uint16_t);
  if (header->free_start + slot_size > header->free_end - cell_size) {
    buffer_pool_->UnpinPage(root_page_id_, false);
    throw std::runtime_error("leaf page full");
  }

  // Add to the slot array, increment cell_count by 1, increment free_start by 2
  // (16 bits), decrement free_end by cell_size. Insert cell there, calling
  // WritePage? Look for where in the slot array to insert the new slot, then
  // insert.
  //
  // I'm seeing the merits of having something like a lightweight RAII guard for
  // each page that most importantly manages the page lifetime, but also manages
  // read and write access to the page when we go multithreaded. When the guard
  // goes out of scope, the pincount is decremented.

  auto new_cell_offset =
      static_cast<std::uint16_t>(header->free_end - cell_size);
  // Write cell bytes at page + new_cell_offset
  char *cell = page + new_cell_offset;
  auto cell_header = CellHeader{
      .key_size = static_cast<std::uint16_t>(key.size()),
      .value_size = static_cast<std::uint16_t>(value.size()),
      .flags = 0,
  };

  // memcpy the cell header to the cell
  std::memcpy(cell, &cell_header, sizeof(cell_header));
  // memcpy the key bytes and value bytes after this
  std::memcpy(cell + sizeof(CellHeader), key.data(), key.size());
  std::memcpy(cell + sizeof(CellHeader) + key.size(), value.data(),
              value.size());

  if (!found) {
    for (std::uint16_t i = header->cell_count; i > index; i--) {
      slots[i] = slots[i - 1];
    }
    header->cell_count++;
    header->free_start += sizeof(std::uint16_t);
  }
  slots[index] = new_cell_offset;

  header->free_end = new_cell_offset;

  // Unpin page after modifying in memory
  buffer_pool_->UnpinPage(root_page_id_, true);
}

void BTree::Remove(std::string_view key) {
  // We don't remove, just mark it as tombstoned
  auto *page = buffer_pool_->FetchPage(root_page_id_);
  auto *header = reinterpret_cast<LeafHeader *>(page);
  auto *slots = reinterpret_cast<uint16_t *>(page + sizeof(LeafHeader));
  bool dirty = false;

  // Do a linear search for now to find the key
  for (std::uint16_t i = 0; i < header->cell_count; i++) {
    char *cell = page + slots[i];
    auto *cell_header = reinterpret_cast<CellHeader *>(cell);
    char *cell_key = cell + sizeof(CellHeader);
    std::string_view cell_key_view(cell_key, cell_header->key_size);

    if (cell_key_view == key) {
      cell_header->flags = 1;
      dirty = true;
      break;
    }

    if (cell_key_view > key) {
      break;
    }
  }

  buffer_pool_->UnpinPage(root_page_id_, dirty);
}

auto BTree::Scan(std::string_view start, std::string_view end)
    -> std::vector<std::pair<std::string, std::string>> {
  auto results = std::vector<std::pair<std::string, std::string>>{};
  auto *page = buffer_pool_->FetchPage(root_page_id_);
  auto *header = reinterpret_cast<LeafHeader *>(page);
  auto *slots = reinterpret_cast<std::uint16_t *>(page + sizeof(LeafHeader));

  for (std::uint16_t i = 0; i < header->cell_count; i++) {
    char *cell = page + slots[i];
    auto *cell_header = reinterpret_cast<CellHeader *>(cell);
    char *cell_key = cell + sizeof(CellHeader);
    std::string_view cell_key_view(cell_key, cell_header->key_size);

    // Not reached the start yet
    if (cell_key_view < start) {
      continue;
    }

    // Reached the end
    if (cell_key_view >= end) {
      break;
    }

    // Deleted key
    if (cell_header->flags == 1) {
      continue;
    }

    char *value = cell_key + cell_header->key_size;
    results.emplace_back(std::string(cell_key, cell_header->key_size),
                         std::string(value, cell_header->value_size));
  }

  buffer_pool_->UnpinPage(root_page_id_, false);
  return results;
}
}  // namespace tinydb
