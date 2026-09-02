#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/storage/page_codec.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb::btree {

namespace {

struct PathEntry {
  storage::PageId page_id;
  std::size_t child_index;
};

struct PendingSplit {
  std::vector<std::string> separators;
  std::vector<storage::PageId> page_ids;
};

struct PendingWrite {
  storage::PageId page_id;
  storage::PageBytes page;
};

struct PreparedChange {
  std::vector<PendingWrite> writes;
  std::optional<PendingSplit> split;
};

auto LeafLowerBound(const storage::LeafPageView &leaf,
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

auto LeafLowerBound(std::span<const storage::LeafEntry> entries,
                    std::string_view key) noexcept -> std::size_t {
  const auto position =
      std::lower_bound(entries.begin(), entries.end(), key,
                       [](const storage::LeafEntry &entry,
                          std::string_view target) {
                         return entry.key < target;
                       });
  return static_cast<std::size_t>(position - entries.begin());
}

auto ChildIndex(const storage::InternalPageView &page,
                std::string_view key) noexcept -> std::size_t {
  std::size_t first = 0;
  std::size_t count = page.EntryCount();

  while (count != 0) {
    const std::size_t step = count / 2;
    const std::size_t middle = first + step;
    if (key < page.Entry(middle).key) {
      count = step;
    } else {
      first = middle + 1;
      count -= step + 1;
    }
  }

  return first;
}

auto ChildAt(const storage::InternalPageView &page,
             std::size_t index) noexcept -> storage::PageId {
  if (index == 0) {
    return page.LeftmostChild();
  }
  return page.Entry(index - 1).right_child;
}

auto EncodeInternalRange(storage::PageId page_id,
                         storage::PageId leftmost_child,
                         std::span<const storage::InternalEntry> entries,
                         std::size_t first, std::size_t last)
    -> Result<storage::PageBytes> {
  assert(first < last);
  assert(last <= entries.size());
  const storage::PageId first_child =
      first == 0 ? leftmost_child : entries[first - 1].right_child;
  return storage::EncodeInternalPage(page_id, first_child,
                                     entries.subspan(first, last - first));
}

auto LeafEntrySize(const storage::LeafEntry &entry) noexcept -> std::size_t {
  return 2 * sizeof(std::uint16_t) + entry.key.size() + entry.value.size();
}

auto InternalEntrySize(const storage::InternalEntry &entry) noexcept
    -> std::size_t {
  return 2 * sizeof(std::uint16_t) + sizeof(storage::PageId) +
         entry.key.size();
}

auto Difference(std::size_t left, std::size_t right) noexcept -> std::size_t {
  return left > right ? left - right : right - left;
}

auto FindLeafPartitions(storage::PageId page_id,
                        std::span<const storage::LeafEntry> entries,
                        std::size_t inserted_index)
    -> Result<std::vector<std::size_t>> {
  if (entries.size() < 2) {
    return Err(Status::InvalidArgument(
        "key and value do not fit in a leaf page"));
  }

  std::size_t total_size = 0;
  for (const auto &entry : entries) {
    if (!storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                                 std::span{&entry, 1})) {
      return Err(Status::InvalidArgument(
          "key and value do not fit in a leaf page"));
    }
    total_size += LeafEntrySize(entry);
  }

  std::size_t best_split = 1;
  std::size_t best_difference = std::numeric_limits<std::size_t>::max();
  std::size_t left_size = 0;
  for (std::size_t split = 1; split < entries.size(); ++split) {
    left_size += LeafEntrySize(entries[split - 1]);
    const std::size_t difference =
        Difference(left_size, total_size - left_size);
    if (difference < best_difference) {
      best_split = split;
      best_difference = difference;
    }
  }

  if (storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                              entries.first(best_split)) &&
      storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                              entries.subspan(best_split))) {
    return std::vector<std::size_t>{best_split};
  }

  if (inserted_index != 0 && inserted_index + 1 < entries.size() &&
      storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                              entries.first(inserted_index)) &&
      storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                              entries.subspan(inserted_index, 1)) &&
      storage::EncodeLeafPage(page_id, storage::INVALID_PAGE_ID,
                              entries.subspan(inserted_index + 1))) {
    return std::vector<std::size_t>{inserted_index, inserted_index + 1};
  }

  return Err(Status::ResourceExhausted(
      "leaf records cannot be divided between pages"));
}

auto FindInternalSplit(storage::PageId page_id,
                       storage::PageId leftmost_child,
                       std::span<const storage::InternalEntry> entries)
    -> Result<std::size_t> {
  if (entries.size() < 3) {
    return Err(Status::ResourceExhausted(
        "internal entries cannot be divided between two pages"));
  }

  std::size_t total_size = 0;
  for (const auto &entry : entries) {
    total_size += InternalEntrySize(entry);
  }

  std::optional<std::size_t> best_split;
  std::size_t best_difference = std::numeric_limits<std::size_t>::max();
  std::size_t left_size = InternalEntrySize(entries[0]);
  for (std::size_t split = 1; split + 1 < entries.size(); ++split) {
    if (EncodeInternalRange(page_id, leftmost_child, entries, 0, split) &&
        EncodeInternalRange(page_id, leftmost_child, entries, split + 1,
                            entries.size())) {
      const std::size_t right_size =
          total_size - left_size - InternalEntrySize(entries[split]);
      const std::size_t difference = Difference(left_size, right_size);
      if (difference < best_difference) {
        best_split = split;
        best_difference = difference;
      }
    }
    left_size += InternalEntrySize(entries[split]);
  }

  if (!best_split) {
    return Err(Status::ResourceExhausted(
        "internal entries cannot be divided between two pages"));
  }
  return *best_split;
}

auto AllocatePages(cache::BufferPool &buffer_pool, std::size_t count)
    -> Result<std::vector<storage::PageId>> {
  std::vector<storage::PageId> page_ids;
  page_ids.reserve(count);
  while (page_ids.size() < count) {
    auto page_id = buffer_pool.AllocatePage();
    if (!page_id) {
      return Err(std::move(page_id.error()));
    }
    page_ids.push_back(*page_id);
  }
  return page_ids;
}

auto EncodeRoot(storage::PageId root_page_id, const PendingSplit &split)
    -> Result<storage::PageBytes> {
  assert(split.page_ids.size() == split.separators.size() + 1);
  std::vector<storage::InternalEntry> entries;
  entries.reserve(split.separators.size());
  for (std::size_t index = 0; index < split.separators.size(); ++index) {
    entries.push_back({split.separators[index], split.page_ids[index + 1]});
  }
  return storage::EncodeInternalPage(root_page_id, split.page_ids.front(),
                                     entries);
}

auto PrepareLeafSplit(cache::BufferPool &buffer_pool,
                      storage::PageId root_page_id, storage::PageId page_id,
                      storage::PageId next_leaf,
                      std::span<const storage::LeafEntry> entries,
                      std::size_t inserted_index) -> Result<PreparedChange> {
  auto partitions = FindLeafPartitions(page_id, entries, inserted_index);
  if (!partitions) {
    return Err(std::move(partitions.error()));
  }

  const bool is_root = page_id == root_page_id;
  const std::size_t page_count = partitions->size() + 1;
  auto allocated =
      AllocatePages(buffer_pool, is_root ? page_count : page_count - 1);
  if (!allocated) {
    return Err(std::move(allocated.error()));
  }

  PendingSplit split;
  if (!is_root) {
    split.page_ids.push_back(page_id);
  }
  split.page_ids.insert(split.page_ids.end(), allocated->begin(),
                        allocated->end());

  PreparedChange change;
  std::size_t first_entry = 0;
  for (std::size_t part = 0; part < page_count; ++part) {
    const std::size_t last_entry =
        part < partitions->size() ? (*partitions)[part] : entries.size();
    const storage::PageId following_leaf =
        part + 1 < page_count ? split.page_ids[part + 1] : next_leaf;
    auto page = storage::EncodeLeafPage(
        split.page_ids[part], following_leaf,
        entries.subspan(first_entry, last_entry - first_entry));
    assert(page);
    change.writes.push_back({split.page_ids[part], std::move(*page)});
    if (part != 0) {
      split.separators.emplace_back(entries[first_entry].key);
    }
    first_entry = last_entry;
  }

  if (!is_root) {
    change.split = std::move(split);
    return change;
  }

  auto root = EncodeRoot(root_page_id, split);
  if (!root) {
    return Err(std::move(root.error()));
  }
  change.writes.push_back({root_page_id, std::move(*root)});
  return change;
}

auto PrepareLeafPut(cache::BufferPool &buffer_pool,
                    storage::PageId root_page_id, storage::PageId page_id,
                    std::string_view key, std::string_view value)
    -> Result<PreparedChange> {
  auto page = buffer_pool.ReadPage(page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }
  auto leaf = storage::DecodeLeafPage(page_id, page->Bytes());
  if (!leaf) {
    return Err(std::move(leaf.error()));
  }

  std::vector<storage::LeafEntry> entries;
  entries.reserve(leaf->EntryCount() + 1);
  for (std::size_t index = 0; index < leaf->EntryCount(); ++index) {
    entries.push_back(leaf->Entry(index));
  }

  const std::size_t inserted_index = LeafLowerBound(entries, key);
  if (inserted_index < entries.size() && entries[inserted_index].key == key) {
    entries[inserted_index].value = value;
  } else {
    entries.insert(entries.begin() +
                       static_cast<std::ptrdiff_t>(inserted_index),
                   storage::LeafEntry{key, value});
  }

  auto encoded = storage::EncodeLeafPage(page_id, leaf->NextLeaf(), entries);
  if (encoded) {
    PreparedChange change;
    change.writes.push_back({page_id, std::move(*encoded)});
    return change;
  }

  return PrepareLeafSplit(buffer_pool, root_page_id, page_id,
                          leaf->NextLeaf(), entries, inserted_index);
}

auto PrepareParentInsert(cache::BufferPool &buffer_pool,
                         storage::PageId root_page_id, const PathEntry &path,
                         const PendingSplit &child_split, bool is_root)
    -> Result<PreparedChange> {
  auto page = buffer_pool.ReadPage(path.page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }
  auto parent = storage::DecodeInternalPage(path.page_id, page->Bytes());
  if (!parent) {
    return Err(std::move(parent.error()));
  }

  assert(!child_split.page_ids.empty());
  assert(path.child_index <= parent->EntryCount());
  assert(ChildAt(*parent, path.child_index) == child_split.page_ids.front());

  std::vector<storage::InternalEntry> entries;
  entries.reserve(parent->EntryCount() + child_split.separators.size());
  for (std::size_t index = 0; index < parent->EntryCount(); ++index) {
    entries.push_back(parent->Entry(index));
  }

  std::vector<storage::InternalEntry> separators;
  separators.reserve(child_split.separators.size());
  for (std::size_t index = 0; index < child_split.separators.size(); ++index) {
    separators.push_back(
        {child_split.separators[index], child_split.page_ids[index + 1]});
  }
  entries.insert(entries.begin() +
                     static_cast<std::ptrdiff_t>(path.child_index),
                 separators.begin(), separators.end());

  const storage::PageId leftmost_child = parent->LeftmostChild();
  auto encoded =
      storage::EncodeInternalPage(path.page_id, leftmost_child, entries);
  if (encoded) {
    PreparedChange change;
    change.writes.push_back({path.page_id, std::move(*encoded)});
    return change;
  }

  auto split_index = FindInternalSplit(path.page_id, leftmost_child, entries);
  if (!split_index) {
    return Err(std::move(split_index.error()));
  }

  auto allocated = AllocatePages(buffer_pool, is_root ? 2 : 1);
  if (!allocated) {
    return Err(std::move(allocated.error()));
  }

  PendingSplit split;
  split.separators.emplace_back(entries[*split_index].key);
  if (is_root) {
    split.page_ids = std::move(*allocated);
  } else {
    split.page_ids = {path.page_id, allocated->front()};
  }

  auto left = EncodeInternalRange(split.page_ids[0], leftmost_child, entries,
                                  0, *split_index);
  auto right = EncodeInternalRange(split.page_ids[1], leftmost_child, entries,
                                   *split_index + 1, entries.size());
  assert(left && right);

  PreparedChange change;
  change.writes.push_back({split.page_ids[0], std::move(*left)});
  change.writes.push_back({split.page_ids[1], std::move(*right)});
  if (is_root) {
    auto root = EncodeRoot(root_page_id, split);
    assert(root);
    change.writes.push_back({root_page_id, std::move(*root)});
  } else {
    change.split = std::move(split);
  }
  return change;
}

void AppendWrites(std::vector<PendingWrite> &destination,
                  std::vector<PendingWrite> source) {
  for (auto &write : source) {
    destination.push_back(std::move(write));
  }
}

auto WritePages(cache::BufferPool &buffer_pool,
                const std::vector<PendingWrite> &writes) -> Status {
  for (const auto &write : writes) {
    auto status = buffer_pool.WritePage(write.page_id, write.page);
    if (!status.Ok()) {
      return status;
    }
  }
  return {};
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
  storage::PageId page_id = root_page_id_;

  while (true) {
    auto page = buffer_pool_.ReadPage(page_id);
    if (!page) {
      return Err(std::move(page.error()));
    }

    switch (storage::PeekPageType(page->Bytes())) {
    case storage::PageType::Leaf: {
      auto leaf = storage::DecodeLeafPage(page_id, page->Bytes());
      if (!leaf) {
        return Err(std::move(leaf.error()));
      }

      const std::size_t index = LeafLowerBound(*leaf, key);
      if (index == leaf->EntryCount()) {
        return std::optional<std::string>{};
      }

      const auto entry = leaf->Entry(index);
      if (entry.key != key) {
        return std::optional<std::string>{};
      }
      return std::optional<std::string>{std::string{entry.value}};
    }
    case storage::PageType::Internal: {
      auto internal = storage::DecodeInternalPage(page_id, page->Bytes());
      if (!internal) {
        return Err(std::move(internal.error()));
      }
      page_id = ChildAt(*internal, ChildIndex(*internal, key));
      break;
    }
    }
  }
}

Status BPlusTree::Put(std::string_view key, std::string_view value) {
  std::vector<PathEntry> path;
  storage::PageId page_id = root_page_id_;

  while (true) {
    auto page = buffer_pool_.ReadPage(page_id);
    if (!page) {
      return std::move(page.error());
    }

    switch (storage::PeekPageType(page->Bytes())) {
    case storage::PageType::Leaf:
      break;
    case storage::PageType::Internal: {
      auto internal = storage::DecodeInternalPage(page_id, page->Bytes());
      if (!internal) {
        return std::move(internal.error());
      }
      const std::size_t child_index = ChildIndex(*internal, key);
      path.push_back({page_id, child_index});
      page_id = ChildAt(*internal, child_index);
      continue;
    }
    }
    break;
  }

  auto change = PrepareLeafPut(buffer_pool_, root_page_id_, page_id, key,
                               value);
  if (!change) {
    return std::move(change.error());
  }

  std::vector<PendingWrite> writes;
  AppendWrites(writes, std::move(change->writes));
  while (change->split && !path.empty()) {
    const PathEntry parent = path.back();
    path.pop_back();
    change = PrepareParentInsert(buffer_pool_, root_page_id_, parent,
                                 *change->split, path.empty());
    if (!change) {
      return std::move(change.error());
    }
    AppendWrites(writes, std::move(change->writes));
  }

  if (change->split) {
    return Status::Corruption("B+ tree path ended before split propagation");
  }
  return WritePages(buffer_pool_, writes);
}

} // namespace tinydb::btree
