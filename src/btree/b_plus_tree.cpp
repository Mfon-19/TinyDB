#include "tinydb/btree/b_plus_tree.h"
#include "tinydb/storage/page_codec.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb::btree {

namespace {

using storage::InternalEntry;
using storage::LeafEntry;
using storage::PageId;

inline constexpr std::size_t MAX_ENTRY_SIZE = storage::PAGE_SIZE / 4;

auto Entries(const storage::LeafPageView &leaf) -> std::vector<LeafEntry> {
  std::vector<LeafEntry> entries;
  entries.reserve(leaf.EntryCount() + 1);
  for (std::size_t index = 0; index < leaf.EntryCount(); ++index) {
    entries.push_back(leaf.Entry(index));
  }
  return entries;
}

auto Entries(const storage::InternalPageView &page)
    -> std::vector<InternalEntry> {
  std::vector<InternalEntry> entries;
  entries.reserve(page.EntryCount() + 1);
  for (std::size_t index = 0; index < page.EntryCount(); ++index) {
    entries.push_back(page.Entry(index));
  }
  return entries;
}

auto EntrySize(const LeafEntry &entry) noexcept -> std::size_t {
  return 2 * sizeof(std::uint16_t) + entry.key.size() + entry.value.size();
}

auto EntrySize(const InternalEntry &entry) noexcept -> std::size_t {
  return 2 * sizeof(std::uint16_t) + sizeof(PageId) + entry.key.size();
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

auto ChildIndex(std::span<const InternalEntry> entries,
                std::string_view key) noexcept -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::upper_bound(entries, key, {}, &InternalEntry::key) -
      entries.begin());
}

auto ChildAt(const storage::InternalPageView &page,
             std::span<const InternalEntry> entries,
             std::size_t index) noexcept -> PageId {
  return index == 0 ? page.LeftmostChild() : entries[index - 1].right_child;
}

auto ReadPageCopy(cache::BufferPool &buffer_pool,
                  PageId page_id) -> Result<storage::PageBytes> {
  auto page = buffer_pool.ReadPage(page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }
  return page->Bytes();
}

} // namespace

Status BPlusTree::Initialize() {
  auto page =
      storage::EncodeLeafPage(root_page_id_, storage::INVALID_PAGE_ID, {});
  if (!page) {
    return std::move(page.error());
  }
  return buffer_pool_.WritePage(root_page_id_, *page);
}

auto BPlusTree::FindLeaf(std::string_view key) -> Result<PageId> {
  PageId page_id = root_page_id_;

  while (true) {
    auto page = buffer_pool_.ReadPage(page_id);
    if (!page) {
      return Err(std::move(page.error()));
    }
    if (storage::PeekPageType(page->Bytes()) != storage::PageType::Internal) {
      return page_id;
    }

    auto internal = storage::DecodeInternalPage(page_id, page->Bytes());
    if (!internal) {
      return Err(std::move(internal.error()));
    }
    const auto entries = Entries(*internal);
    page_id = ChildAt(*internal, entries, ChildIndex(entries, key));
  }
}

auto BPlusTree::Get(std::string_view key)
    -> Result<std::optional<std::string>> {
  auto page_id = FindLeaf(key);
  if (!page_id) {
    return Err(std::move(page_id.error()));
  }
  auto page = buffer_pool_.ReadPage(*page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }
  auto leaf = storage::DecodeLeafPage(*page_id, page->Bytes());
  if (!leaf) {
    return Err(std::move(leaf.error()));
  }

  const auto entries = Entries(*leaf);
  const auto found =
      std::ranges::lower_bound(entries, key, {}, &LeafEntry::key);
  if (found == entries.end() || found->key != key) {
    return std::nullopt;
  }
  return std::string{found->value};
}

Status BPlusTree::Put(std::string_view key, std::string_view value) {
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
  return buffer_pool_.WritePage(root_page_id_, *root);
}

auto BPlusTree::Delete(std::string_view key) -> Result<bool> {
  return Remove(root_page_id_, key);
}

auto BPlusTree::Seek(std::string_view key) -> Result<Cursor> {
  Cursor cursor(*this);
  if (auto status = cursor.Position(key, true); !status.Ok()) {
    return Err(std::move(status));
  }
  return cursor;
}

Status BPlusTree::RebuildFreeList() {
  const PageId page_count = buffer_pool_.PageCount();
  std::vector<bool> reachable(page_count, false);
  std::vector<PageId> pending{root_page_id_};

  while (!pending.empty()) {
    const PageId page_id = pending.back();
    pending.pop_back();
    if (page_id == 0 || page_id >= page_count || reachable[page_id]) {
      return Status::Corruption("invalid or repeated B+ tree page ID");
    }
    reachable[page_id] = true;

    auto page = ReadPageCopy(buffer_pool_, page_id);
    if (!page) {
      return std::move(page.error());
    }
    if (storage::PeekPageType(*page) != storage::PageType::Internal) {
      auto leaf = storage::DecodeLeafPage(page_id, *page);
      if (!leaf) {
        return std::move(leaf.error());
      }
      continue;
    }

    auto internal = storage::DecodeInternalPage(page_id, *page);
    if (!internal) {
      return std::move(internal.error());
    }

    pending.push_back(internal->LeftmostChild());
    for (std::size_t index = 0; index < internal->EntryCount(); ++index) {
      pending.push_back(internal->Entry(index).right_child);
    }
  }

  std::vector<PageId> free_pages;
  for (PageId page_id = 1; page_id < page_count; ++page_id) {
    if (!reachable[page_id]) {
      free_pages.push_back(page_id);
    }
  }
  buffer_pool_.SetFreePages(std::move(free_pages));
  return {};
}

auto BPlusTree::Remove(PageId page_id, std::string_view key) -> Result<bool> {
  auto page = ReadPageCopy(buffer_pool_, page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }

  if (storage::PeekPageType(*page) == storage::PageType::Internal) {
    auto internal = storage::DecodeInternalPage(page_id, *page);
    if (!internal) {
      return Err(std::move(internal.error()));
    }

    auto entries = Entries(*internal);
    const std::size_t child_index = ChildIndex(entries, key);
    auto child = Remove(ChildAt(*internal, entries, child_index), key);
    if (!child) {
      return Err(std::move(child.error()));
    }
    if (!*child) {
      return false;
    }

    if (entries.size() == 1 && page_id != root_page_id_) {
      return true;
    }

    // Merge with the right sibling, or the left one for the last child.
    const std::size_t left_index = std::min(child_index, entries.size() - 1);
    const PageId left = ChildAt(*internal, entries, left_index);
    const PageId right = ChildAt(*internal, entries, left_index + 1);
    const PageId target = entries.size() == 1 ? page_id : left;
    auto merged = MergeChildren(target, left, right, entries[left_index].key);
    if (!merged) {
      return Err(std::move(merged.error()));
    }
    if (!*merged) {
      return true;
    }

    buffer_pool_.FreePage(right);
    if (target == page_id) {
      buffer_pool_.FreePage(left);
      return true;
    }
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(left_index));

    auto encoded = storage::EncodeInternalPage(
        page_id, internal->LeftmostChild(), entries);
    if (!encoded) {
      return Err(std::move(encoded.error()));
    }
    if (auto status = buffer_pool_.WritePage(page_id, *encoded);
        !status.Ok()) {
      return Err(std::move(status));
    }
    return true;
  }

  auto leaf = storage::DecodeLeafPage(page_id, *page);
  if (!leaf) {
    return Err(std::move(leaf.error()));
  }

  auto entries = Entries(*leaf);
  const auto found =
      std::ranges::lower_bound(entries, key, {}, &LeafEntry::key);
  if (found == entries.end() || found->key != key) {
    return false;
  }
  entries.erase(found);

  auto encoded = storage::EncodeLeafPage(page_id, leaf->NextLeaf(), entries);
  if (!encoded) {
    return Err(std::move(encoded.error()));
  }
  if (auto status = buffer_pool_.WritePage(page_id, *encoded); !status.Ok()) {
    return Err(std::move(status));
  }
  return true;
}

auto BPlusTree::MergeChildren(PageId target, PageId left_id, PageId right_id,
                              std::string_view separator) -> Result<bool> {
  auto left = ReadPageCopy(buffer_pool_, left_id);
  if (!left) {
    return Err(std::move(left.error()));
  }
  auto right = ReadPageCopy(buffer_pool_, right_id);
  if (!right) {
    return Err(std::move(right.error()));
  }

  Result<storage::PageBytes> merged;
  if (storage::PeekPageType(*left) == storage::PageType::Internal) {
    auto left_page = storage::DecodeInternalPage(left_id, *left);
    if (!left_page) {
      return Err(std::move(left_page.error()));
    }
    auto right_page = storage::DecodeInternalPage(right_id, *right);
    if (!right_page) {
      return Err(std::move(right_page.error()));
    }

    auto entries = Entries(*left_page);
    entries.push_back({separator, right_page->LeftmostChild()});
    const auto right_entries = Entries(*right_page);
    entries.insert(entries.end(), right_entries.begin(), right_entries.end());
    merged = storage::EncodeInternalPage(target, left_page->LeftmostChild(),
                                         entries);
  } else {
    auto left_page = storage::DecodeLeafPage(left_id, *left);
    if (!left_page) {
      return Err(std::move(left_page.error()));
    }
    auto right_page = storage::DecodeLeafPage(right_id, *right);
    if (!right_page) {
      return Err(std::move(right_page.error()));
    }

    auto entries = Entries(*left_page);
    const auto right_entries = Entries(*right_page);
    entries.insert(entries.end(), right_entries.begin(), right_entries.end());
    merged = storage::EncodeLeafPage(target, right_page->NextLeaf(), entries);
  }

  if (!merged) {
    return false;
  }
  if (auto status = buffer_pool_.WritePage(target, *merged); !status.Ok()) {
    return Err(std::move(status));
  }
  return true;
}

auto BPlusTree::Insert(PageId page_id, std::string_view key,
                       std::string_view value) -> Result<std::optional<Split>> {
  auto page = buffer_pool_.ReadPage(page_id);
  if (!page) {
    return Err(std::move(page.error()));
  }

  if (storage::PeekPageType(page->Bytes()) == storage::PageType::Internal) {
    auto internal = storage::DecodeInternalPage(page_id, page->Bytes());
    if (!internal) {
      return Err(std::move(internal.error()));
    }

    auto entries = Entries(*internal);
    const std::size_t child_index = ChildIndex(entries, key);
    auto child = Insert(ChildAt(*internal, entries, child_index), key, value);
    if (!child) {
      return Err(std::move(child.error()));
    }
    if (!*child) {
      return std::nullopt;
    }

    const Split &split = **child;
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(child_index),
                   InternalEntry{split.separator, split.right});
    if (auto encoded = storage::EncodeInternalPage(
            page_id, internal->LeftmostChild(), entries)) {
      if (auto status = buffer_pool_.WritePage(page_id, *encoded);
          !status.Ok()) {
        return Err(std::move(status));
      }
      return std::nullopt;
    }
    return SplitInternal(page_id, internal->LeftmostChild(), entries);
  }

  auto leaf = storage::DecodeLeafPage(page_id, page->Bytes());
  if (!leaf) {
    return Err(std::move(leaf.error()));
  }

  auto entries = Entries(*leaf);
  auto position = std::ranges::lower_bound(entries, key, {}, &LeafEntry::key);
  if (position != entries.end() && position->key == key) {
    position->value = value;
  } else {
    entries.insert(position, LeafEntry{key, value});
  }
  if (auto encoded =
          storage::EncodeLeafPage(page_id, leaf->NextLeaf(), entries)) {
    if (auto status = buffer_pool_.WritePage(page_id, *encoded); !status.Ok()) {
      return Err(std::move(status));
    }
    return std::nullopt;
  }
  return SplitLeaf(page_id, leaf->NextLeaf(), entries);
}

auto BPlusTree::SplitLeaf(PageId page_id, PageId next_leaf,
                          std::span<const LeafEntry> entries) -> Result<Split> {
  const std::size_t index = SplitIndex(entries);
  auto split = AllocateSplit(page_id, entries[index].key);
  if (!split) {
    return Err(std::move(split.error()));
  }

  auto left =
      storage::EncodeLeafPage(split->left, split->right, entries.first(index));
  auto right =
      storage::EncodeLeafPage(split->right, next_leaf, entries.subspan(index));
  return WriteSplit(std::move(*split), std::move(left), std::move(right));
}

auto BPlusTree::SplitInternal(PageId page_id, PageId leftmost_child,
                              std::span<const InternalEntry> entries)
    -> Result<Split> {
  const std::size_t index = SplitIndex(entries);
  const InternalEntry &separator = entries[index - 1];
  auto split = AllocateSplit(page_id, separator.key);
  if (!split) {
    return Err(std::move(split.error()));
  }

  auto left = storage::EncodeInternalPage(split->left, leftmost_child,
                                          entries.first(index - 1));
  auto right = storage::EncodeInternalPage(split->right, separator.right_child,
                                           entries.subspan(index));
  return WriteSplit(std::move(*split), std::move(left), std::move(right));
}

auto BPlusTree::AllocateSplit(PageId page_id,
                              std::string_view separator) -> Result<Split> {
  Split split{page_id, std::string{separator}, storage::INVALID_PAGE_ID};
  if (page_id == root_page_id_) {
    auto left = buffer_pool_.AllocatePage();
    if (!left) {
      return Err(std::move(left.error()));
    }
    split.left = *left;
  }

  auto right = buffer_pool_.AllocatePage();
  if (!right) {
    return Err(std::move(right.error()));
  }
  split.right = *right;
  return split;
}

auto BPlusTree::WriteSplit(Split split, Result<storage::PageBytes> left,
                           Result<storage::PageBytes> right) -> Result<Split> {
  if (!left) {
    return Err(std::move(left.error()));
  }
  if (!right) {
    return Err(std::move(right.error()));
  }
  if (auto status = buffer_pool_.WritePage(split.right, *right); !status.Ok()) {
    return Err(std::move(status));
  }
  if (auto status = buffer_pool_.WritePage(split.left, *left); !status.Ok()) {
    return Err(std::move(status));
  }
  return split;
}

auto Cursor::Key() const noexcept -> std::string_view {
  assert(Valid());
  return leaf_->Entry(index_).key;
}

auto Cursor::Value() const noexcept -> std::string_view {
  assert(Valid());
  return leaf_->Entry(index_).value;
}

Status Cursor::Next() {
  assert(Valid());
  if (index_ + 1 < leaf_->EntryCount()) {
    ++index_;
    return {};
  }

  const std::string last_key{Key()};
  return Position(last_key, false);
}

Status Cursor::Position(std::string_view key, bool inclusive) {
  leaf_.reset();
  auto page_id = tree_->FindLeaf(key);
  if (!page_id) {
    return std::move(page_id.error());
  }

  for (PageId leaf_id = *page_id; leaf_id != storage::INVALID_PAGE_ID;) {
    auto page = tree_->buffer_pool_.ReadPage(leaf_id);
    if (!page) {
      return std::move(page.error());
    }
    *page_ = page->Bytes();
    auto leaf = storage::DecodeLeafPage(leaf_id, *page_);
    if (!leaf) {
      return std::move(leaf.error());
    }

    const auto entries = Entries(*leaf);
    auto found = std::ranges::lower_bound(entries, key, {}, &LeafEntry::key);
    if (!inclusive && found != entries.end() && found->key == key) {
      ++found;
    }
    if (found != entries.end()) {
      leaf_ = *leaf;
      index_ = static_cast<std::size_t>(found - entries.begin());
      return {};
    }
    leaf_id = leaf->NextLeaf();
  }
  return {};
}

} // namespace tinydb::btree
