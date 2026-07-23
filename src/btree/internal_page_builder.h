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
  struct SplitResult;

  InternalPageBuilder() = default;
  InternalPageBuilder(page_id_t first_child, std::string_view separator, page_id_t right_child);

  static auto From(const InternalPageView &page) -> InternalPageBuilder;

  // Emits canonical transaction bytes. Commit assigns the final LSN and CRC.
  void Store(char *page, page_id_t page_id) const;

  void InsertSeparator(std::string_view key, page_id_t right_child);

  auto Fits() const -> bool;

  auto Split() -> SplitResult;

 private:
  struct Slice {
    std::size_t offset;
    std::size_t size;
  };

  struct Record {
    Slice key;
    page_id_t right_child;
  };

  auto Append(std::string_view bytes) -> Slice;
  auto MakeRecord(std::string_view key, page_id_t right_child) -> Record;
  auto Key(const Record &record) const -> std::string_view;
  static auto RecordFootprint(const Record &record) -> std::size_t;
  auto ChooseSplitIndex() const -> std::size_t;

  page_id_t first_child_{HEADER_PAGE_ID};
  std::vector<char> bytes_;
  std::vector<Record> records_;
  std::size_t encoded_bytes_{0};
};

struct InternalPageBuilder::SplitResult {
  // separator moves to the parent; its old right child begins right.
  InternalPageBuilder right;
  std::string separator;
};

}  // namespace tinydb
