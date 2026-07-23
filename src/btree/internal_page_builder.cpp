#include "storage/page.h"
#include "util/check.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "internal_page_builder.h"
#include "page_format.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "txn/contract.h"

/*
** INTERNAL PAGE MUTATION
**
** InternalPageBuilder is private routing-page state. N ordered separators
** describe N+1 children: first_child_ owns keys below the first separator and
** each record owns the child on its right. Persistent bytes enter only through
** InternalPageView.
**
** Store emits one complete canonical page. A split promotes one separator to
** the parent; the promoted key belongs to neither resulting child and its old
** right child becomes the new right page's first child.
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - INTERNAL_HEADER_SIZE;

auto KeyIsBefore(const InternalPageBuilder::Record &record, std::string_view target) -> bool {
  return txn::BytewiseLess{}(record.key, target);
}

// Store adds exactly one slot and one unpadded cell per separator.
auto RecordFootprint(const InternalPageBuilder::Record &record) -> std::size_t {
  return SLOT_SIZE + INTERNAL_CELL_HEADER_SIZE + record.key.size();
}

}  // namespace

// Construct the minimal internal page used when a split creates a new root.
InternalPageBuilder::InternalPageBuilder(page_id_t first_child, std::string separator, page_id_t right_child)
    : first_child_(first_child) {
  auto record = Record{std::move(separator), right_child};
  encoded_bytes_ = RecordFootprint(record);
  records_.push_back(std::move(record));
}

// Copy only through view accessors so builders cannot become a second decoder.
auto InternalPageBuilder::From(const InternalPageView &page) -> InternalPageBuilder {
  InternalPageBuilder builder;
  builder.first_child_ = page.ChildAt(0);
  builder.records_.reserve(page.SeparatorCount());
  for (std::size_t index = 0; index < page.SeparatorCount(); ++index) {
    auto record = Record{std::string(page.KeyAt(index)), page.ChildAt(index + 1)};
    builder.encoded_bytes_ += RecordFootprint(record);
    builder.records_.push_back(std::move(record));
  }
  return builder;
}

// Reinitialize the complete page, append slots in separator order, and pack
// cells from the end downward. The header link stores first_child_.
void InternalPageBuilder::Store(char *page, page_id_t page_id) const {
  TINYDB_CHECK(Fits(), "records do not fit in a page");
  auto bytes = std::as_writable_bytes(std::span{page, PAGE_SIZE});
  TINYDB_CHECK(
      storage::InitializeDataPage(bytes, storage::DataPageType::Internal, page_id, 0,
                                  static_cast<std::uint16_t>(PAGE_SIZE - storage::data_page_offset::HEADER_BYTES))
          .Ok(),
      "failed to initialize internal page");

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const std::size_t cell_size = INTERNAL_CELL_HEADER_SIZE + record.key.size();
    const std::size_t offset = free_end - cell_size;
    TINYDB_CHECK(storage::PutLittleEndian(bytes, offset + internal_cell_offset::RIGHT_CHILD, record.right_child) &&
                     storage::PutLittleEndian(bytes, offset + internal_cell_offset::KEY_BYTES,
                                              static_cast<std::uint16_t>(record.key.size())),
                 "internal cell header exceeds page");
    std::copy_n(record.key.data(), record.key.size(), page + offset + INTERNAL_CELL_HEADER_SIZE);
    TINYDB_CHECK(storage::PutLittleEndian(bytes, INTERNAL_HEADER_SIZE + i * SLOT_SIZE, static_cast<slot_t>(offset)),
                 "internal slot exceeds page");
    free_end = offset;
  }

  TINYDB_CHECK(
      storage::PutLittleEndian(bytes, node_page_offset::CELL_COUNT, static_cast<std::uint16_t>(records_.size())) &&
          storage::PutLittleEndian(bytes, node_page_offset::FREE_START,
                                   static_cast<std::uint16_t>(INTERNAL_HEADER_SIZE + records_.size() * SLOT_SIZE)) &&
          storage::PutLittleEndian(bytes, node_page_offset::FREE_END, static_cast<std::uint16_t>(free_end)) &&
          storage::PutLittleEndian(bytes, node_page_offset::RESERVED, std::uint16_t{0}) &&
          storage::PutLittleEndian(bytes, node_page_offset::LINK, first_child_) &&
          storage::FinalizeDataPage(bytes).Ok(),
      "failed to encode internal page");
}

void InternalPageBuilder::InsertSeparator(std::string key, page_id_t right_child) {
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  auto record = Record{std::move(key), right_child};
  encoded_bytes_ += RecordFootprint(record);
  records_.insert(it, std::move(record));
}

auto InternalPageBuilder::Fits() const -> bool { return encoded_bytes_ <= USABLE_BYTES; }

// Choose the legal promoted separator with the smallest encoded-size
// imbalance. The candidate itself belongs to neither child.
auto InternalPageBuilder::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 3, "too few records to split");

  // The first and last separators cannot be promoted because each resulting
  // internal page must retain at least one routing record. Keep the left size
  // incrementally instead of allocating a prefix array.
  std::size_t best_split = 0;
  std::size_t best_imbalance = 0;
  std::size_t left = RecordFootprint(records_.front());
  bool found = false;
  for (std::size_t split = 1; split + 1 < count; ++split) {
    const std::size_t right = encoded_bytes_ - left - RecordFootprint(records_[split]);
    if (left > USABLE_BYTES || right > USABLE_BYTES) {
      left += RecordFootprint(records_[split]);
      continue;
    }
    const std::size_t imbalance = left > right ? left - right : right - left;
    if (!found || imbalance < best_imbalance) {
      found = true;
      best_split = split;
      best_imbalance = imbalance;
    }
    left += RecordFootprint(records_[split]);
  }
  TINYDB_CHECK(found, "no valid split point; entry size cap violated");
  return best_split;
}

auto InternalPageBuilder::Split() -> SplitResult {
  // Promotion removes the separator from both children. Its former right child
  // becomes the right page's first child, preserving every subtree edge.
  const std::size_t split = ChooseSplitIndex();
  const auto split_it = records_.begin() + static_cast<std::ptrdiff_t>(split);
  const auto promoted_bytes = RecordFootprint(records_[split]);

  SplitResult result;
  result.separator = std::move(records_[split].key);
  result.right.first_child_ = records_[split].right_child;
  result.right.records_.assign(std::make_move_iterator(split_it + 1), std::make_move_iterator(records_.end()));
  for (const auto &record : result.right.records_) {
    result.right.encoded_bytes_ += RecordFootprint(record);
  }
  TINYDB_CHECK(encoded_bytes_ >= promoted_bytes + result.right.encoded_bytes_,
               "internal split byte accounting underflow");
  encoded_bytes_ -= promoted_bytes + result.right.encoded_bytes_;
  records_.erase(split_it, records_.end());
  return result;
}

}  // namespace tinydb
