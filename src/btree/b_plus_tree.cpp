#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/storage/page_codec.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb::btree {

namespace {

using storage::InternalEntry;
using storage::Keys;
using storage::LeafEntry;
using storage::PageId;

auto Entries(const auto &page) {
  std::vector<decltype(page.Entry(0))> entries;
  entries.reserve(page.EntryCount() + 1);
  for (std::size_t index = 0; index < page.EntryCount(); ++index) {
    entries.push_back(page.Entry(index));
  }
  return entries;
}

template <typename Entry>
auto SplitIndex(std::span<const Entry> entries) noexcept -> std::size_t {
  std::size_t total = 0;
  for (const auto &entry : entries) {
    total += EntrySize(entry);
  }

  std::size_t index = 0;
  for (std::size_t left = 0; left < total / 2; ++index) {
    left += EntrySize(entries[index]);
  }
  return index;
}

auto ChildIndex(const storage::InternalPageView &page,
                std::string_view key) noexcept -> std::size_t {
  const auto keys = Keys(page);
  return static_cast<std::size_t>(std::ranges::upper_bound(keys, key) -
                                  keys.begin());
}

auto ChildAt(const storage::InternalPageView &page,
             std::size_t index) noexcept -> PageId {
  return index == 0 ? page.LeftmostChild() : page.Entry(index - 1).right_child;
}

} // namespace

auto BPlusTree::Initialize() -> Status {
  auto page =
      storage::EncodeLeafPage(root_page_id_, storage::INVALID_PAGE_ID, {});
  if (!page) {
    return std::move(page.error());
  }
  return context_.WritePage(*page);
}

auto BPlusTree::FindLeaf(std::string_view key) -> Result<storage::Page> {
  PageId page_id = root_page_id_;

  while (true) {
    auto page = context_.ReadPage(page_id);
    if (!page) {
      return Err(std::move(page.error()));
    }
    if (page->Type() == storage::PageType::Leaf) {
      return page;
    }

    const auto internal = page->Internal();
    page_id = ChildAt(internal, ChildIndex(internal, key));
  }
}

auto BPlusTree::Get(std::string_view key)
    -> Result<std::optional<std::string>> {
  auto page = FindLeaf(key);
  if (!page) {
    return Err(std::move(page.error()));
  }

  const auto leaf = page->Leaf();
  const auto keys = Keys(leaf);
  const auto found = std::ranges::lower_bound(keys, key);
  if (found == keys.end() || *found != key) {
    return std::nullopt;
  }
  return std::string{
      leaf.Entry(static_cast<std::size_t>(found - keys.begin())).value};
}

auto BPlusTree::Put(std::string_view key, std::string_view value) -> Status {
  if (key.size() + value.size() > MAX_ENTRY_SIZE) {
    return Status::InvalidArgument("key and value are too large");
  }

  auto result = Insert(root_page_id_, key, value);
  if (!result) {
    return std::move(result.error());
  }
  if (!*result) {
    return {};
  }

  const Split &split = **result;
  const InternalEntry entry{split.separator, split.right};
  auto root = storage::EncodeInternalPage(root_page_id_, split.left,
                                          std::span{&entry, 1});
  if (!root) {
    return std::move(root.error());
  }
  return context_.WritePage(*root);
}

auto BPlusTree::Delete(std::string_view key) -> Result<bool> {
  return Remove(root_page_id_, key);
}

auto BPlusTree::Seek(std::string_view key) -> Result<Cursor> {
  auto page = FindLeaf(key);
  if (!page) {
    return Err(std::move(page.error()));
  }
  Cursor cursor(context_, *page);
  if (auto status = cursor.Position(key); !status.Ok()) {
    return Err(std::move(status));
  }
  return cursor;
}

auto BPlusTree::FindFreePages(PageId page_count)
    -> Result<std::vector<PageId>> {
  std::vector<bool> reachable(page_count, false);
  std::vector<PageId> pending{root_page_id_};

  while (!pending.empty()) {
    const PageId page_id = pending.back();
    pending.pop_back();
    if (page_id == 0 || page_id >= page_count || reachable[page_id]) {
      return Err(Status::Corruption("invalid or repeated B+ tree page ID"));
    }
    reachable[page_id] = true;

    auto page = context_.ReadPage(page_id);
    if (!page) {
      return Err(std::move(page.error()));
    }
    if (page->Type() == storage::PageType::Leaf) {
      continue;
    }

    const auto internal = page->Internal();
    pending.push_back(internal.LeftmostChild());
    for (std::size_t index = 0; index < internal.EntryCount(); ++index) {
      pending.push_back(internal.Entry(index).right_child);
    }
  }

  std::vector<PageId> free_pages;
  for (PageId page_id = 1; page_id < page_count; ++page_id) {
    if (!reachable[page_id]) {
      free_pages.push_back(page_id);
    }
  }
  return free_pages;
}

auto BPlusTree::Remove(PageId page_id, std::string_view key) -> Result<bool> {
  auto page = context_.ReadPage(page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }

  if (page->Type() == storage::PageType::Internal) {
    const auto internal = page->Internal();
    const std::size_t child_index = ChildIndex(internal, key);
    auto child = Remove(ChildAt(internal, child_index), key);
    if (!child) {
      return Err(std::move(child.error()));
    }
    if (!*child) {
      return false;
    }

    if (internal.EntryCount() == 1 && page_id != root_page_id_) {
      return true;
    }

    // Merge with the right sibling, or the left one for the last child.
    const std::size_t left_index =
        std::min(child_index, internal.EntryCount() - 1);
    const PageId left = ChildAt(internal, left_index);
    const PageId right = ChildAt(internal, left_index + 1);
    const PageId target = internal.EntryCount() == 1 ? page_id : left;
    auto merged =
        MergeChildren(target, left, right, internal.Entry(left_index).key);
    if (!merged) {
      return Err(std::move(merged.error()));
    }
    if (!*merged) {
      return true;
    }

    context_.FreePage(right);
    if (target == page_id) {
      context_.FreePage(left);
      return true;
    }
    auto entries = Entries(internal);
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(left_index));

    auto encoded =
        storage::EncodeInternalPage(page_id, internal.LeftmostChild(), entries);
    if (!encoded) {
      return Err(std::move(encoded.error()));
    }
    if (auto status = context_.WritePage(*encoded); !status.Ok()) {
      return Err(std::move(status));
    }
    return true;
  }

  const auto leaf = page->Leaf();
  const auto keys = Keys(leaf);
  const auto found = std::ranges::lower_bound(keys, key);
  if (found == keys.end() || *found != key) {
    return false;
  }
  auto entries = Entries(leaf);
  entries.erase(entries.begin() +
                static_cast<std::ptrdiff_t>(found - keys.begin()));

  auto encoded = storage::EncodeLeafPage(page_id, leaf.NextLeaf(), entries);
  if (!encoded) {
    return Err(std::move(encoded.error()));
  }
  if (auto status = context_.WritePage(*encoded); !status.Ok()) {
    return Err(std::move(status));
  }
  return true;
}

auto BPlusTree::MergeChildren(PageId target, PageId left_id, PageId right_id,
                              std::string_view separator) -> Result<bool> {
  auto left = context_.ReadPage(left_id);
  if (!left) {
    return Err(std::move(left.error()));
  }
  auto right = context_.ReadPage(right_id);
  if (!right) {
    return Err(std::move(right.error()));
  }
  if (left->Type() != right->Type()) {
    return Err(Status::Corruption("cannot merge pages of different types"));
  }
  auto added_size = right->PayloadSize();
  if (left->Type() == storage::PageType::Internal) {
    added_size += EntrySize(InternalEntry{separator, right_id});
  }
  if (added_size > left->FreeSpace()) {
    return false;
  }

  auto merged = [&]() -> Result<storage::Page> {
    if (left->Type() == storage::PageType::Internal) {
      const auto left_page = left->Internal();
      const auto right_page = right->Internal();
      auto entries = Entries(left_page);
      entries.push_back({separator, right_page.LeftmostChild()});
      for (std::size_t index = 0; index < right_page.EntryCount(); ++index) {
        entries.push_back(right_page.Entry(index));
      }
      return storage::EncodeInternalPage(target, left_page.LeftmostChild(),
                                         entries);
    }
    const auto left_page = left->Leaf();
    const auto right_page = right->Leaf();
    auto entries = Entries(left_page);
    for (std::size_t index = 0; index < right_page.EntryCount(); ++index) {
      entries.push_back(right_page.Entry(index));
    }
    return storage::EncodeLeafPage(target, right_page.NextLeaf(), entries);
  }();
  if (!merged) {
    return Err(std::move(merged.error()));
  }
  if (auto status = context_.WritePage(*merged); !status.Ok()) {
    return Err(std::move(status));
  }
  return true;
}

auto BPlusTree::Insert(PageId page_id, std::string_view key,
                       std::string_view value) -> Result<std::optional<Split>> {
  auto page = context_.ReadPage(page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }

  if (page->Type() == storage::PageType::Internal) {
    const auto internal = page->Internal();
    const std::size_t child_index = ChildIndex(internal, key);
    auto child = Insert(ChildAt(internal, child_index), key, value);
    if (!child) {
      return Err(std::move(child.error()));
    }
    if (!*child) {
      return std::nullopt;
    }

    const Split &split = **child;
    auto entries = Entries(internal);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(child_index),
                   InternalEntry{split.separator, split.right});
    if (storage::EntriesFit(std::span<const InternalEntry>{entries})) {
      auto encoded = storage::EncodeInternalPage(
          page_id, internal.LeftmostChild(), entries);
      if (!encoded) {
        return Err(std::move(encoded.error()));
      }
      if (auto status = context_.WritePage(*encoded); !status.Ok()) {
        return Err(std::move(status));
      }
      return std::nullopt;
    }
    return SplitInternal(page_id, internal.LeftmostChild(), entries);
  }

  const auto leaf = page->Leaf();
  const auto keys = Keys(leaf);
  const auto found = std::ranges::lower_bound(keys, key);
  const auto index = static_cast<std::size_t>(found - keys.begin());
  const bool exists = found != keys.end() && *found == key;
  if (exists && leaf.Entry(index).value == value) {
    return std::nullopt;
  }
  auto entries = Entries(leaf);
  auto position = entries.begin() + static_cast<std::ptrdiff_t>(index);
  if (exists) {
    position->value = value;
  } else {
    entries.insert(position, LeafEntry{key, value});
  }
  if (storage::EntriesFit(std::span<const LeafEntry>{entries})) {
    auto encoded = storage::EncodeLeafPage(page_id, leaf.NextLeaf(), entries);
    if (!encoded) {
      return Err(std::move(encoded.error()));
    }
    if (auto status = context_.WritePage(*encoded); !status.Ok()) {
      return Err(std::move(status));
    }
    return std::nullopt;
  }
  return SplitLeaf(page_id, leaf.NextLeaf(), entries);
}

auto BPlusTree::SplitLeaf(PageId page_id, PageId next_leaf,
                          std::span<const LeafEntry> entries) -> Result<Split> {
  const std::size_t index = SplitIndex(entries);
  assert(index > 0 && index < entries.size());
  auto split = AllocateSplit(page_id, entries[index].key);
  if (!split) {
    return Err(std::move(split.error()));
  }

  auto left =
      storage::EncodeLeafPage(split->left, split->right, entries.first(index));
  if (!left) {
    return Err(std::move(left.error()));
  }
  auto right =
      storage::EncodeLeafPage(split->right, next_leaf, entries.subspan(index));
  if (!right) {
    return Err(std::move(right.error()));
  }
  return WriteSplit(std::move(*split), *left, *right);
}

auto BPlusTree::SplitInternal(PageId page_id, PageId leftmost_child,
                              std::span<const InternalEntry> entries)
    -> Result<Split> {
  const std::size_t index = SplitIndex(entries);
  assert(index > 1 && index < entries.size());
  const InternalEntry &separator = entries[index - 1];
  auto split = AllocateSplit(page_id, separator.key);
  if (!split) {
    return Err(std::move(split.error()));
  }

  auto left = storage::EncodeInternalPage(split->left, leftmost_child,
                                          entries.first(index - 1));
  if (!left) {
    return Err(std::move(left.error()));
  }
  auto right = storage::EncodeInternalPage(split->right, separator.right_child,
                                           entries.subspan(index));
  if (!right) {
    return Err(std::move(right.error()));
  }
  return WriteSplit(std::move(*split), *left, *right);
}

auto BPlusTree::AllocateSplit(PageId page_id,
                              std::string_view separator) -> Result<Split> {
  Split split{page_id, std::string{separator}, storage::INVALID_PAGE_ID};
  if (page_id == root_page_id_) {
    auto left = context_.AllocatePage();
    if (!left) {
      return Err(std::move(left.error()));
    }
    split.left = *left;
  }

  auto right = context_.AllocatePage();
  if (!right) {
    return Err(std::move(right.error()));
  }
  split.right = *right;
  return split;
}

auto BPlusTree::WriteSplit(Split split, const storage::Page &left,
                           const storage::Page &right) -> Result<Split> {
  if (auto status = context_.WritePage(right); !status.Ok()) {
    return Err(std::move(status));
  }
  if (auto status = context_.WritePage(left); !status.Ok()) {
    return Err(std::move(status));
  }
  return split;
}

} // namespace tinydb::btree
