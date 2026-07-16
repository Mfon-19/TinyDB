#include "util/check.h"
#include "storage/page.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
** right child becomes the new right page's first child. Absorb performs the
** inverse operation by pulling a parent separator between adjacent children.
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - INTERNAL_HEADER_SIZE;

// Occupancy is measured in encoded bytes. Pulling a parent separator into two
// sparse internal pages can still exceed one page, so repair may redistribute.
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES / 2;

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
  records_.push_back(Record{std::move(separator), right_child});
}

// Copy only through view accessors so builders cannot become a second decoder.
auto InternalPageBuilder::From(const InternalPageView &page) -> InternalPageBuilder {
  InternalPageBuilder builder;
  builder.first_child_ = page.ChildAt(0);
  builder.records_.reserve(page.SeparatorCount());
  for (std::size_t index = 0; index < page.SeparatorCount(); ++index) {
    builder.records_.push_back(Record{std::string(page.KeyAt(index)), page.ChildAt(index + 1)});
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

auto InternalPageBuilder::ChildAt(std::size_t child_index) const -> page_id_t {
  TINYDB_CHECK(child_index <= records_.size(), "child index out of range");
  return child_index == 0 ? first_child_ : records_[child_index - 1].right_child;
}

auto InternalPageBuilder::SeparatorKeyAt(std::size_t index) const -> const std::string & {
  TINYDB_CHECK(index < records_.size(), "separator index out of range");
  return records_[index].key;
}

void InternalPageBuilder::SetSeparatorKey(std::size_t index, std::string key) {
  TINYDB_CHECK(index < records_.size(), "separator index out of range");
  records_[index].key = std::move(key);
}

void InternalPageBuilder::InsertSeparator(std::string key, page_id_t right_child) {
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  records_.insert(it, Record{std::move(key), right_child});
}

void InternalPageBuilder::EraseSeparator(std::size_t index) {
  TINYDB_CHECK(index < records_.size(), "separator index out of range");
  records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(index));
}

auto InternalPageBuilder::Bytes() const -> std::size_t {
  std::size_t total = 0;
  for (const auto &record : records_) {
    total += RecordFootprint(record);
  }
  return total;
}

auto InternalPageBuilder::Fits() const -> bool { return Bytes() <= USABLE_BYTES; }

auto InternalPageBuilder::Underfull() const -> bool { return Bytes() < MIN_FILL_BYTES; }

// Choose the legal promoted separator with the smallest encoded-size
// imbalance. The candidate itself belongs to neither child.
auto InternalPageBuilder::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 3, "too few records to split");

  auto prefix = std::vector<std::size_t>(count + 1, 0);
  for (std::size_t i = 0; i < count; ++i) {
    prefix[i + 1] = prefix[i] + RecordFootprint(records_[i]);
  }

  // The first and last separators cannot be promoted because each resulting
  // internal page must retain at least one routing record.
  std::size_t best_split = 0;
  std::size_t best_imbalance = 0;
  bool found = false;
  for (std::size_t split = 1; split + 1 < count; ++split) {
    const std::size_t left = prefix[split];
    const std::size_t right = prefix[count] - prefix[split + 1];
    if (left > USABLE_BYTES || right > USABLE_BYTES) {
      continue;
    }
    const std::size_t imbalance = left > right ? left - right : right - left;
    if (!found || imbalance < best_imbalance) {
      found = true;
      best_split = split;
      best_imbalance = imbalance;
    }
  }
  TINYDB_CHECK(found, "no valid split point; entry size cap violated");
  return best_split;
}

auto InternalPageBuilder::Split() -> SplitResult {
  // Promotion removes the separator from both children. Its former right child
  // becomes the right page's first child, preserving every subtree edge.
  const std::size_t split = ChooseSplitIndex();
  const auto split_it = records_.begin() + static_cast<std::ptrdiff_t>(split);

  SplitResult result;
  result.separator = std::move(records_[split].key);
  result.right.first_child_ = records_[split].right_child;
  result.right.records_.assign(std::make_move_iterator(split_it + 1), std::make_move_iterator(records_.end()));
  records_.erase(split_it, records_.end());
  return result;
}

void InternalPageBuilder::Absorb(std::string separator, InternalPageBuilder &&right) {
  // The parent separator bridges the siblings: its right child is the right
  // page's former first child. The caller may keep this merge or split it again.
  TINYDB_CHECK(records_.empty() || txn::BytewiseLess{}(records_.back().key, separator),
               "separator does not sort after this node's keys");
  records_.push_back(Record{std::move(separator), right.first_child_});
  records_.insert(records_.end(), std::make_move_iterator(right.records_.begin()),
                  std::make_move_iterator(right.records_.end()));
}

}  // namespace tinydb
