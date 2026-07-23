#include "btree/page_source.h"

#include "util/check.h"

#include <utility>

namespace tinydb {

auto PageHandle::MutableData() -> char * {
  TINYDB_CHECK(mutable_data_ != nullptr, "mutable access through a read-only page handle");
  return mutable_data_;
}

void PageHandle::MarkDirty() {
  TINYDB_CHECK(mutable_data_ != nullptr, "marking a read-only page handle dirty");
  dirty_ = true;
}

void PageHandle::Reset() noexcept {
  // The source receives the accumulated dirty bit exactly once.
  if (release_ != nullptr) {
    release_(owner_, page_id_, dirty_);
  }
  owner_ = nullptr;
  data_ = nullptr;
  mutable_data_ = nullptr;
  release_ = nullptr;
  keepalive_.reset();
  validated_header_ = nullptr;
  tree_payload_validated_ = false;
}

void PageHandle::Take(PageHandle &&other) noexcept {
  // Page bytes do not move; only responsibility for the lease does.
  owner_ = other.owner_;
  page_id_ = other.page_id_;
  data_ = other.data_;
  mutable_data_ = other.mutable_data_;
  dirty_ = other.dirty_;
  release_ = other.release_;
  keepalive_ = std::move(other.keepalive_);
  validated_header_ = other.validated_header_;
  tree_payload_validated_ = other.tree_payload_validated_;

  // Ownership moves with the callback; the source object must release nothing.
  other.owner_ = nullptr;
  other.data_ = nullptr;
  other.mutable_data_ = nullptr;
  other.release_ = nullptr;
  other.dirty_ = false;
  other.validated_header_ = nullptr;
  other.tree_payload_validated_ = false;
}

}  // namespace tinydb
