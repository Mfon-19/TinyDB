#include "tinydb/cursor.h"
#include "tinydb/detail/page_context.h"
#include <algorithm>
#include <cassert>
#include <utility>

namespace tinydb {

Cursor::Cursor(detail::PageContext &context, const storage::Page &page)
    : context_(&context), page_(std::make_unique<storage::Page>(page)),
      version_(context.Version()) {}

auto Cursor::Valid() const noexcept -> bool {
  return page_ && context_->Active() && context_->Version() == version_;
}

auto Cursor::Key() const noexcept -> std::string_view {
  assert(Valid());
  return page_->Leaf().Entry(index_).key;
}

auto Cursor::Value() const noexcept -> std::string_view {
  assert(Valid());
  return page_->Leaf().Entry(index_).value;
}

auto Cursor::Next() -> Status {
  if (auto status = context_->CheckActive(); !status.Ok()) {
    return status;
  }
  if (!Valid()) {
    return Status::InvalidArgument("cursor is not valid");
  }
  const auto leaf = page_->Leaf();
  if (++index_ < leaf.EntryCount()) {
    return {};
  }
  return LoadLeaf(leaf.NextLeaf());
}

auto Cursor::Position(std::string_view key) -> Status {
  while (page_) {
    const auto leaf = page_->Leaf();
    const auto keys = storage::Keys(leaf);
    const auto found = std::ranges::lower_bound(keys, key);
    if (found != keys.end()) {
      index_ = static_cast<std::size_t>(found - keys.begin());
      return {};
    }
    if (auto status = LoadLeaf(leaf.NextLeaf()); !status.Ok()) {
      return status;
    }
  }
  return {};
}

auto Cursor::LoadLeaf(storage::PageId page_id) -> Status {
  while (page_id != storage::INVALID_PAGE_ID) {
    auto page = context_->ReadPage(page_id);
    if (!page) {
      page_.reset();
      return context_->Fail(std::move(page.error()));
    }
    if (page->Type() != storage::PageType::Leaf) {
      page_.reset();
      return context_->Fail(
          Status::Corruption("leaf link points to an internal page"));
    }
    *page_ = std::move(*page);
    const auto leaf = page_->Leaf();
    if (leaf.EntryCount() != 0) {
      index_ = 0;
      return {};
    }
    page_id = leaf.NextLeaf();
  }
  page_.reset();
  return {};
}

}
