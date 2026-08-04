#include "storage/page.h"
#include "util/check.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "leaf_page_builder.h"
#include "page_format.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "txn/contract.h"

/*
** LEAF PAGE MUTATION
**
** LeafPageBuilder is private mutation state. It copies logical records from
** one validated view, preserves strict key order and the successor link, and
** emits one canonical packed private page. It never interprets persistent
** offsets.
**
** Every Store rebuilds the complete page, which removes fragmentation and
** stale bytes. The checksum remains zero while the page is private; commit
** assigns the exact LSN and seals it once. Splits choose an encoded-byte
** boundary rather than a record count because values vary in size. The
** separator copied to the parent is the right page's minimum key and remains
** present in that leaf.
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - LEAF_HEADER_SIZE;

}  // namespace

auto LeafPageBuilder::Append(std::string_view bytes) -> Slice {
  if (bytes_.capacity() == 0) {
    // One insertion may temporarily overflow a full page before splitting.
    // Offsets remain stable even if a nonstandard caller grows beyond this
    // ordinary one-allocation budget.
    bytes_.reserve(PAGE_SIZE + MAX_LEAF_RECORD_BYTES);
  }
  const auto result = Slice{.offset = bytes_.size(), .size = bytes.size()};
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  return result;
}

auto LeafPageBuilder::MakeRecord(std::string_view key, LeafValueView value) -> Record {
  auto record = Record{
      .key = Append(key),
      .inline_value = {},
      .overflow = {},
      .kind = value.Kind(),
  };
  if (value.IsOverflow()) {
    record.overflow = value.OverflowDescriptor();
  } else {
    record.inline_value = Append(value.InlineBytes());
  }
  return record;
}

auto LeafPageBuilder::Key(const Record &record) const -> std::string_view {
  return {bytes_.data() + record.key.offset, record.key.size};
}

auto LeafPageBuilder::Value(const Record &record) const -> LeafValueView {
  if (record.kind == LeafValueKind::Overflow) {
    return LeafValueView::Overflow(record.overflow);
  }
  return LeafValueView::Inline(std::string_view{bytes_.data() + record.inline_value.offset, record.inline_value.size});
}

auto LeafPageBuilder::RecordFootprint(const Record &record) -> std::size_t {
  const auto value_bytes =
      record.kind == LeafValueKind::Overflow ? OVERFLOW_VALUE_DESCRIPTOR_BYTES : record.inline_value.size;
  return SLOT_SIZE + LEAF_CELL_HEADER_SIZE + record.key.size + value_bytes;
}

// Copy only through view accessors so builders cannot become a second decoder.
auto LeafPageBuilder::From(const LeafPageView &page) -> LeafPageBuilder {
  LeafPageBuilder builder;
  builder.next_leaf_ = page.NextLeaf();
  // A builder is opened for exactly one mutation. Leave room for that insert
  // so a full leaf needs one record allocation, not one while decoding and
  // another immediately afterward.
  builder.records_.reserve(page.Count() + 1);
  for (std::size_t index = 0; index < page.Count(); ++index) {
    auto record = builder.MakeRecord(page.KeyAt(index), page.ValueAt(index));
    builder.encoded_bytes_ += RecordFootprint(record);
    builder.records_.push_back(record);
  }
  return builder;
}

// Reinitialize the complete page, append slots in key order, and pack cells
// from the end downward. Rebuilding removes fragmentation and stale bytes.
void LeafPageBuilder::Store(char *page, page_id_t page_id) const {
  TINYDB_CHECK(Fits(), "records do not fit in a page");
  auto bytes = std::as_writable_bytes(std::span<char, PAGE_SIZE>{page, PAGE_SIZE});
  storage::InitializeDataPage(bytes, storage::DataPageType::Leaf, page_id, 0,
                              static_cast<std::uint16_t>(PAGE_SIZE - storage::data_page_offset::HEADER_BYTES));

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const auto key = Key(record);
    const auto value = Value(record);
    const auto value_bytes = value.IsOverflow() ? OVERFLOW_VALUE_DESCRIPTOR_BYTES : value.InlineBytes().size();
    const std::size_t cell_size = LEAF_CELL_HEADER_SIZE + key.size() + value_bytes;
    const std::size_t offset = free_end - cell_size;
    storage::PutLittleEndianUnchecked(bytes, offset + leaf_cell_offset::KEY_BYTES,
                                      static_cast<std::uint16_t>(key.size()));
    storage::PutLittleEndianUnchecked(bytes, offset + leaf_cell_offset::VALUE_BYTES,
                                      static_cast<std::uint16_t>(value_bytes));
    bytes[offset + leaf_cell_offset::VALUE_KIND] = static_cast<std::byte>(value.Kind());
    std::copy_n(key.data(), key.size(), page + offset + LEAF_CELL_HEADER_SIZE);
    const auto value_offset = offset + LEAF_CELL_HEADER_SIZE + key.size();
    if (value.IsOverflow()) {
      const auto &descriptor = value.OverflowDescriptor();
      storage::PutLittleEndianUnchecked(bytes, value_offset + overflow_descriptor_offset::TOTAL_VALUE_BYTES,
                                        descriptor.total_value_bytes);
      storage::PutLittleEndianUnchecked(bytes, value_offset + overflow_descriptor_offset::FIRST_PAGE_ID,
                                        descriptor.first_page_id);
    } else {
      const auto inline_bytes = value.InlineBytes();
      std::copy_n(inline_bytes.data(), inline_bytes.size(), page + value_offset);
    }
    storage::PutLittleEndianUnchecked(bytes, LEAF_HEADER_SIZE + i * SLOT_SIZE, static_cast<slot_t>(offset));
    free_end = offset;
  }

  storage::PutLittleEndianUnchecked(bytes, node_page_offset::CELL_COUNT, static_cast<std::uint16_t>(records_.size()));
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::FREE_START,
                                    static_cast<std::uint16_t>(LEAF_HEADER_SIZE + records_.size() * SLOT_SIZE));
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::FREE_END, static_cast<std::uint16_t>(free_end));
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::RESERVED, std::uint16_t{0});
  storage::PutLittleEndianUnchecked(bytes, node_page_offset::LINK, next_leaf_);
}

auto LeafPageBuilder::Upsert(std::string_view key, LeafValueView value) -> UpsertResult {
  // at_tail means "key is at the right edge", not "new record". Put combines
  // it with the successor sentinel to select the sequential-load split policy.
  const auto it = std::lower_bound(
      records_.begin(), records_.end(), key,
      [this](const Record &record, std::string_view target) { return txn::BytewiseLess{}(Key(record), target); });
  const bool at_tail = it == records_.end();
  auto replaced_overflow = std::optional<OverflowValueDescriptor>{};
  if (!at_tail && txn::BytewiseCompare(Key(*it), key) == 0) {
    const auto stored = Value(*it);
    if (!stored.IsOverflow() && !value.IsOverflow() && stored.InlineBytes() == value.InlineBytes()) {
      return UpsertResult{.changed = false, .at_tail = false, .replaced_overflow = std::nullopt};
    }
    encoded_bytes_ -= RecordFootprint(*it);
    if (it->kind == LeafValueKind::Overflow) {
      replaced_overflow = it->overflow;
    }
    *it = MakeRecord(key, value);
    encoded_bytes_ += RecordFootprint(*it);
  } else {
    auto record = MakeRecord(key, value);
    encoded_bytes_ += RecordFootprint(record);
    records_.insert(it, record);
  }
  return UpsertResult{
      .changed = true,
      .at_tail = at_tail,
      .replaced_overflow = replaced_overflow,
  };
}

auto LeafPageBuilder::Erase(std::string_view key) -> EraseResult {
  const auto it = std::lower_bound(
      records_.begin(), records_.end(), key,
      [this](const Record &record, std::string_view target) { return txn::BytewiseLess{}(Key(record), target); });
  if (it == records_.end() || txn::BytewiseCompare(Key(*it), key) != 0) {
    return EraseResult{.erased = false, .removed_overflow = std::nullopt};
  }
  auto removed_overflow = std::optional<OverflowValueDescriptor>{};
  if (it->kind == LeafValueKind::Overflow) {
    removed_overflow = it->overflow;
  }
  encoded_bytes_ -= RecordFootprint(*it);
  records_.erase(it);
  return EraseResult{
      .erased = true,
      .removed_overflow = removed_overflow,
  };
}

auto LeafPageBuilder::Fits() const -> bool { return encoded_bytes_ <= USABLE_BYTES; }

// Choose the legal byte boundary with the smallest encoded-size imbalance.
auto LeafPageBuilder::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 2, "too few records to split");

  // Only boundaries where both encoded halves fit are candidates. Among
  // those, prefer the smallest byte imbalance to stabilize occupancy. A
  // running left size avoids allocating a prefix array for this one pass.
  std::size_t best_split = 0;
  std::size_t best_imbalance = 0;
  std::size_t left = 0;
  bool found = false;
  for (std::size_t split = 1; split < count; ++split) {
    left += RecordFootprint(records_[split - 1]);
    const std::size_t right = encoded_bytes_ - left;
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

auto LeafPageBuilder::Split(page_id_t right_page_id, bool tail_heavy) -> SplitResult {
  // A leaf separator is copied from the right minimum; the key itself must
  // remain in the leaf. Tail-heavy mode isolates a newly appended record so a
  // sequential load leaves dense completed pages behind it.
  const bool lopsided = tail_heavy && records_.size() >= 2;
  const std::size_t split = lopsided ? records_.size() - 1 : ChooseSplitIndex();
  const auto split_it = records_.begin() + static_cast<std::ptrdiff_t>(split);

  SplitResult result;
  result.separator = std::string(Key(records_[split]));
  result.right.records_.reserve(records_.size() - split);
  for (auto record = split_it; record != records_.end(); ++record) {
    auto copied = result.right.MakeRecord(Key(*record), Value(*record));
    result.right.encoded_bytes_ += RecordFootprint(copied);
    result.right.records_.push_back(copied);
  }
  TINYDB_CHECK(encoded_bytes_ >= result.right.encoded_bytes_, "leaf split byte accounting underflow");
  encoded_bytes_ -= result.right.encoded_bytes_;
  result.right.next_leaf_ = next_leaf_;
  records_.erase(split_it, records_.end());
  next_leaf_ = right_page_id;  // right is inserted between this leaf and old_next
  return result;
}

}  // namespace tinydb
