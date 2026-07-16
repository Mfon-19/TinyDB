#pragma once

#include <tinydb/page.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "btree/page_view.h"

namespace tinydb {

// Owning mutation state for one routing page. Persistent bytes enter only
// through InternalPageView.
class InternalPageBuilder {
 public:
  struct Record {
    std::string key;
    page_id_t right_child;
  };

  struct SplitResult;

  InternalPageBuilder() = default;
  InternalPageBuilder(page_id_t first_child, std::string separator, page_id_t right_child);

  static auto From(const InternalPageView &page) -> InternalPageBuilder;

  // Emits a complete canonical page whose encoded identity is page_id.
  void Store(char *page, page_id_t page_id) const;

  auto ChildAt(std::size_t child_index) const -> page_id_t;

  auto SeparatorCount() const -> std::size_t { return records_.size(); }
  auto SeparatorKeyAt(std::size_t index) const -> const std::string &;
  void SetSeparatorKey(std::size_t index, std::string key);
  void InsertSeparator(std::string key, page_id_t right_child);
  void EraseSeparator(std::size_t index);

  auto Fits() const -> bool;
  auto Underfull() const -> bool;

  auto Split() -> SplitResult;

  // Pulls the parent separator down between two adjacent child ranges.
  void Absorb(std::string separator, InternalPageBuilder &&right);

  auto FirstChild() const -> page_id_t { return first_child_; }

 private:
  auto Bytes() const -> std::size_t;
  auto ChooseSplitIndex() const -> std::size_t;

  page_id_t first_child_{HEADER_PAGE_ID};
  std::vector<Record> records_;
};

struct InternalPageBuilder::SplitResult {
  // separator moves to the parent; its old right child begins right.
  InternalPageBuilder right;
  std::string separator;
};

}  // namespace tinydb
