#pragma once

#include "storage/page.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "btree/page_view.h"
#include "btree/value.h"

namespace tinydb {

// Owning mutation state for one leaf. Reads use LeafPageView directly.
class LeafPageBuilder {
 public:
  struct Record {
    std::string key;
    LeafValue value;
  };

  struct UpsertResult {
    bool at_tail;
    std::optional<OverflowValueDescriptor> replaced_overflow;
  };

  struct EraseResult {
    bool erased;
    std::optional<OverflowValueDescriptor> removed_overflow;
  };

  struct SplitResult;

  LeafPageBuilder() = default;

  static auto From(const LeafPageView &page) -> LeafPageBuilder;

  // Emits a complete canonical page whose encoded identity is page_id.
  void Store(char *page, page_id_t page_id) const;

  auto Upsert(std::string_view key, LeafValue value) -> UpsertResult;
  auto Erase(std::string_view key) -> EraseResult;
  auto Fits() const -> bool;

  auto Split(page_id_t right_page_id, bool tail_heavy) -> SplitResult;

  auto NextLeaf() const -> page_id_t { return next_leaf_; }

 private:
  auto ChooseSplitIndex() const -> std::size_t;

  page_id_t next_leaf_{HEADER_PAGE_ID};
  std::vector<Record> records_;
  std::size_t encoded_bytes_{0};
};

struct LeafPageBuilder::SplitResult {
  // separator is right's inclusive lower bound and remains in right.
  LeafPageBuilder right;
  std::string separator;
};

}  // namespace tinydb
