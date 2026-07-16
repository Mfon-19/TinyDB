#pragma once

#include <tinydb/page.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "btree/page_view.h"

namespace tinydb {

// Owning mutation state for one leaf. Reads use LeafPageView directly.
class LeafPageBuilder {
 public:
  struct Record {
    std::string key;
    std::string value;
  };

  struct SplitResult;

  LeafPageBuilder() = default;

  static auto From(const LeafPageView &page) -> LeafPageBuilder;

  // Emits a complete canonical page whose encoded identity is page_id.
  void Store(char *page, page_id_t page_id) const;

  auto Upsert(std::string_view key, std::string_view value) -> bool;
  auto Erase(std::string_view key) -> bool;
  auto Fits() const -> bool;
  auto Underfull() const -> bool;

  auto Split(page_id_t right_page_id, bool tail_heavy) -> SplitResult;

  // Appends an adjacent right range and bypasses it in the leaf chain.
  void Absorb(LeafPageBuilder &&right);

  auto Records() const -> const std::vector<Record> & { return records_; }
  auto NextLeaf() const -> page_id_t { return next_leaf_; }

 private:
  auto Bytes() const -> std::size_t;
  auto ChooseSplitIndex() const -> std::size_t;

  page_id_t next_leaf_{HEADER_PAGE_ID};
  std::vector<Record> records_;
};

struct LeafPageBuilder::SplitResult {
  // separator is right's inclusive lower bound and remains in right.
  LeafPageBuilder right;
  std::string separator;
};

}  // namespace tinydb
