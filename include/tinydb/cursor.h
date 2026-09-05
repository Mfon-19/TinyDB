#pragma once

#include "tinydb/storage/page_codec.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace tinydb::detail {
class PageContext;
}
namespace tinydb::btree {
class BPlusTree;
}

namespace tinydb {

/*
 * A Cursor over the leaf chain in the B+ tree
 */
class Cursor {
public:
  Cursor(const Cursor &) = delete;
  auto operator=(const Cursor &) -> Cursor & = delete;
  Cursor(Cursor &&) noexcept = default;
  auto operator=(Cursor &&) noexcept -> Cursor & = default;

  [[nodiscard]] auto Valid() const noexcept -> bool;
  [[nodiscard]] auto Key() const noexcept -> std::string_view;
  [[nodiscard]] auto Value() const noexcept -> std::string_view;
  auto Next() -> Status;

private:
  friend class btree::BPlusTree;
  Cursor(detail::PageContext &context, storage::PageRef page);

  auto Position(std::string_view key) -> Status;
  auto LoadLeaf(storage::PageId page_id) -> Status;

  detail::PageContext *context_;
  storage::PageRef page_;
  storage::LeafPageView leaf_;
  std::uint64_t version_;
  std::size_t index_ = 0;
};

}
