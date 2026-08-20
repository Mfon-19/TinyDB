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
** Store emits one complete canonical private page and leaves checksum sealing
** to commit. A split promotes one separator to the parent; the promoted key
** belongs to neither resulting child and its old right child becomes the new
** right page's first child.
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - INTERNAL_HEADER_SIZE;

}  // namespace
auto InternalPageBuilder::Append(std::string_view bytes) -> Slice {
  if (bytes_.capacity() == 0) {
    bytes_.reserve(PAGE_SIZE + MAX_KEY_BYTES);
  }
  const auto result = Slice{.offset = bytes_.size(), .size = bytes.size()};
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  return result;
}

auto InternalPageBuilder::MakeRecord(std::string_view key, page_id_t right_child) -> Record {
  return Record{.key = Append(key), .right_child = right_child};
}

auto InternalPageBuilder::Key(const Record &record) const -> std::string_view {
  return {bytes_.data() + record.key.offset, record.key.size};
}

auto InternalPageBuilder::RecordFootprint(const Record &record) -> std::size_t {
  return SLOT_SIZE + INTERNAL_CELL_HEADER_SIZE + record.key.size;
}

InternalPageBuilder::InternalPageBuilder(page_id_t first_child, std::string_view separator, page_id_t right_child)
    : first_child_(first_child) {
  auto record = MakeRecord(separator, right_child);
  encoded_bytes_ = RecordFootprint(record);
  records_.push_back(record);
}

/*
** Copy logical records through InternalPageView. Builders must not become a
** second decoder for persistent offsets, because that would create another
** trust boundary beside the page validator.
*/
auto InternalPageBuilder::From(const InternalPageView &page) -> InternalPageBuilder {
  InternalPageBuilder builder;
  builder.first_child_ = page.ChildAt(0);
  builder.records_.reserve(page.SeparatorCount() + 1);
  for (std::size_t index = 0; index < page.SeparatorCount(); ++index) {
    auto record = builder.MakeRecord(page.KeyAt(index), page.ChildAt(index + 1));
    builder.encoded_bytes_ += RecordFootprint(record);
    builder.records_.push_back(record);
  }
  return builder;
}

void InternalPageBuilder::Store(char *page, page_id_t page_id) const {
  TINYDB_CHECK(Fits(), "records do not fit in a page");
  auto bytes = std::as_writable_bytes(std::span<char, PAGE_SIZE>{page, PAGE_SIZE});
  storage::InitializeDataPage(bytes, storage::DataPageType::Internal, page_id, 0,
                              static_cast<std::uint16_t>(PAGE_SIZE - storage::data_page_offset::HEADER_BYTES));

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const auto key = Key(record);
    const std::size_t cell_size = INTERNAL_CELL_HEADER_SIZE + key.size();
    const std::size_t offset = free_end - cell_size;
    storage::PutLittleEndianUnchecked(bytes, offset + internal_cell_offset::RIGHT_CHILD, record.right_child);
    storage::PutLittleEndianUnchecked(bytes, offset + internal_cell_offset::KEY_BYTES,
                                      static_cast<std::uint16_t>(key.size()));
    std::copy_n(key.data(), key.size(), page + offset + INTERNAL_CELL_HEADER_SIZE);
    storage::PutLittleEndianUnchecked(bytes, INTERNAL_HEADER_SIZE + i * SLOT_SIZE, static_cast<slot_t>(offset));
    free_end = offset;
  }

  storage::PutLittleEndianUnchecked(bytes, node_page_offset::CELL_COUNT, static_cast<std::uint16_t>(records_.size()));
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::FREE_START,
                                    static_cast<std::uint16_t>(INTERNAL_HEADER_SIZE + records_.size() * SLOT_SIZE));
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::FREE_END, static_cast<std::uint16_t>(free_end));
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::RESERVED, std::uint16_t{0});
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::LINK, first_child_);
}

void InternalPageBuilder::InsertSeparator(std::string_view key, page_id_t right_child) {
  const auto it = std::lower_bound(
      records_.begin(), records_.end(), key,
      [this](const Record &record, std::string_view target) { return txn::BytewiseLess{}(Key(record), target); });
  auto record = MakeRecord(key, right_child);
  encoded_bytes_ += RecordFootprint(record);
  records_.insert(it, record);
}

auto InternalPageBuilder::Fits() const -> bool { return encoded_bytes_ <= USABLE_BYTES; }

/*
** Return the legal promoted separator with the smallest encoded-size
** imbalance. The first and last separators are not candidates because each
** resulting internal page must retain at least one routing record. The
** promoted separator itself belongs to neither child.
*/
auto InternalPageBuilder::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 3, "too few records to split");

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
  // The promoted separator leaves both pages; its former right child becomes
  // the new page's first child, preserving the edge that crossed the split.
  const std::size_t split = ChooseSplitIndex();
  const auto split_it = records_.begin() + static_cast<std::ptrdiff_t>(split);
  const auto promoted_bytes = RecordFootprint(records_[split]);

  SplitResult result;
  result.separator = std::string(Key(records_[split]));
  result.right.first_child_ = records_[split].right_child;
  result.right.records_.reserve(records_.size() - split - 1);
  for (auto record = split_it + 1; record != records_.end(); ++record) {
    auto copied = result.right.MakeRecord(Key(*record), record->right_child);
    result.right.encoded_bytes_ += RecordFootprint(copied);
    result.right.records_.push_back(copied);
  }
  TINYDB_CHECK(encoded_bytes_ >= promoted_bytes + result.right.encoded_bytes_,
               "internal split byte accounting underflow");
  encoded_bytes_ -= promoted_bytes + result.right.encoded_bytes_;
  records_.erase(split_it, records_.end());
  return result;
}

}  // namespace tinydb
