#include "btree/page_source.h"

#include <tinydb/check.h>

#include <utility>

namespace tinydb {

auto PageHandle::MutableData() -> char * {
  TINYDB_CHECK(editable_, "mutable access through a read-only page handle");
  return data_;
}

void PageHandle::MarkDirty() {
  TINYDB_CHECK(editable_, "marking a read-only page handle dirty");
  dirty_ = true;
}

void PageHandle::Reset() noexcept {
  // The source receives the accumulated dirty bit exactly once.
  if (release_ != nullptr) {
    release_(owner_, page_id_, dirty_);
  }
  owner_ = nullptr;
  data_ = nullptr;
  release_ = nullptr;
}

void PageHandle::Take(PageHandle &&other) noexcept {
  // Page bytes do not move; only responsibility for the lease does.
  owner_ = other.owner_;
  page_id_ = other.page_id_;
  data_ = other.data_;
  editable_ = other.editable_;
  dirty_ = other.dirty_;
  release_ = other.release_;

  // Ownership moves with the callback; the source object must release nothing.
  other.owner_ = nullptr;
  other.data_ = nullptr;
  other.release_ = nullptr;
  other.dirty_ = false;
}

}  // namespace tinydb
