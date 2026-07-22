#pragma once

#include "storage/page.h"

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

  void InsertSeparator(std::string key, page_id_t right_child);

  auto Fits() const -> bool;

  auto Split() -> SplitResult;

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
