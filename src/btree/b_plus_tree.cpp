#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/storage/page_codec.h"
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tinydb::btree {

namespace {

auto LowerBound(const storage::LeafPageView &leaf,
                std::string_view key) noexcept -> std::size_t {
  std::size_t first = 0;
  std::size_t count = leaf.EntryCount();

  while (count != 0) {
    const std::size_t step = count / 2;
    const std::size_t middle = first + step;
    if (leaf.Entry(middle).key < key) {
      first = middle + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }

  return first;
}

} // namespace

Status BPlusTree::Initialize() {
  auto page = storage::EncodeLeafPage(
      root_page_id_, storage::INVALID_PAGE_ID,
      std::span<const storage::LeafEntry>{});
  if (!page) {
    return std::move(page.error());
  }
  return buffer_pool_.WritePage(root_page_id_, *page);
}

auto BPlusTree::Get(std::string_view key)
    -> Result<std::optional<std::string>> {
  auto page = buffer_pool_.ReadPage(root_page_id_);
  if (!page) {
    return Err(std::move(page.error()));
  }

  auto leaf = storage::DecodeLeafPage(root_page_id_, page->Bytes());
  if (!leaf) {
    return Err(std::move(leaf.error()));
  }

  const std::size_t index = LowerBound(*leaf, key);
  if (index == leaf->EntryCount()) {
    return std::optional<std::string>{};
  }

  const auto entry = leaf->Entry(index);
  if (entry.key != key) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{std::string{entry.value}};
}

Status BPlusTree::Put(std::string_view key, std::string_view value) {
  storage::PageBytes encoded_page;
  {
    auto page = buffer_pool_.ReadPage(root_page_id_);
    if (!page) {
      return std::move(page.error());
    }

    auto leaf = storage::DecodeLeafPage(root_page_id_, page->Bytes());
    if (!leaf) {
      return std::move(leaf.error());
    }

    std::vector<storage::LeafEntry> entries;
    entries.reserve(leaf->EntryCount() + 1);
    for (std::size_t index = 0; index < leaf->EntryCount(); ++index) {
      entries.push_back(leaf->Entry(index));
    }

    const std::size_t index = LowerBound(*leaf, key);
    if (index < entries.size() && entries[index].key == key) {
      entries[index].value = value;
    } else {
      entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index),
                     storage::LeafEntry{key, value});
    }

    auto encoded = storage::EncodeLeafPage(
        root_page_id_, storage::INVALID_PAGE_ID, entries);
    if (!encoded) {
      return std::move(encoded.error());
    }
    encoded_page = std::move(*encoded);
  }

  return buffer_pool_.WritePage(root_page_id_, encoded_page);
}

} // namespace tinydb::btree
