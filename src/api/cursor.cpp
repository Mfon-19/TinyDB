#include "tinydb/cursor.h"
#include "tinydb/detail/page_context.h"
#include <algorithm>
#include <cassert>
#include <utility>

namespace tinydb {

Cursor::Cursor(detail::PageContext &context, storage::PageRef page)
    : context_(&context), page_(std::move(page)), leaf_(page_->Leaf()),
      version_(context.Version()) {}

auto Cursor::Valid() const noexcept -> bool {
  return page_ && context_->Active() && context_->Version() == version_;
}

auto Cursor::Key() const noexcept -> std::string_view {
  assert(Valid());
  return leaf_.Key(index_);
}

auto Cursor::Value() const noexcept -> std::string_view {
  assert(Valid());
  return leaf_.Entry(index_).value;
}

auto Cursor::Next() -> Status {
  if (auto status = context_->CheckActive(); !status.Ok()) {
    return status;
  }
  if (!page_ || context_->Version() != version_) {
    return Status::InvalidArgument("cursor is not valid");
  }
  if (++index_ < leaf_.EntryCount()) {
    return {};
  }
  return LoadLeaf(leaf_.NextLeaf());
}

auto Cursor::Position(std::string_view key) -> Status {
  while (page_) {
    const auto keys = storage::Keys(leaf_);
    const auto found = std::ranges::lower_bound(keys, key);
    if (found != keys.end()) {
      index_ = static_cast<std::size_t>(found - keys.begin());
      return {};
    }
    if (auto status = LoadLeaf(leaf_.NextLeaf()); !status.Ok()) {
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
    if ((*page)->Type() != storage::PageType::Leaf) {
      page_.reset();
      return context_->Fail(
          Status::Corruption("leaf link points to an internal page"));
    }
    page_ = std::move(*page);
    leaf_ = page_->Leaf();
    if (leaf_.EntryCount() != 0) {
      index_ = 0;
      return {};
    }
    page_id = leaf_.NextLeaf();
  }
  page_.reset();
  return {};
}

}
