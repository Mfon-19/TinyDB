#include <tinydb/check.h>
#include <tinydb/page.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
** emits one canonical packed page. It never interprets persistent offsets.
**
** Every Store rebuilds the complete page, which removes fragmentation and
** stale bytes. Splits choose an encoded-byte boundary rather than a record
** count because values vary in size. The separator copied to the parent is the
** right page's minimum key and remains present in that leaf.
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - LEAF_HEADER_SIZE;

// Occupancy is measured in encoded bytes because record sizes vary. Underfull
// is a repair hint, not a page-validity requirement.
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES / 2;

auto KeyIsBefore(const LeafPageBuilder::Record &record, std::string_view target) -> bool {
  return txn::BytewiseLess{}(record.key, target);
}

// Store adds exactly one slot and one unpadded cell per logical record.
auto RecordFootprint(const LeafPageBuilder::Record &record) -> std::size_t {
  return SLOT_SIZE + LEAF_CELL_HEADER_SIZE + record.key.size() + record.value.size();
}

}  // namespace

// Copy only through view accessors so builders cannot become a second decoder.
auto LeafPageBuilder::From(const LeafPageView &page) -> LeafPageBuilder {
  LeafPageBuilder builder;
  builder.next_leaf_ = page.NextLeaf();
  builder.records_.reserve(page.Count());
  for (std::size_t index = 0; index < page.Count(); ++index) {
    builder.records_.push_back(Record{std::string(page.KeyAt(index)), std::string(page.ValueAt(index))});
  }
  return builder;
}

// Reinitialize the complete page, append slots in key order, and pack cells
// from the end downward. Rebuilding removes fragmentation and stale bytes.
void LeafPageBuilder::Store(char *page, page_id_t page_id) const {
  TINYDB_CHECK(Fits(), "records do not fit in a page");
  auto bytes = std::as_writable_bytes(std::span{page, PAGE_SIZE});
  TINYDB_CHECK(
      storage::InitializeDataPage(bytes, storage::DataPageType::Leaf, page_id, 0,
                                  static_cast<std::uint16_t>(PAGE_SIZE - storage::data_page_offset::HEADER_BYTES))
          .Ok(),
      "failed to initialize leaf page");

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const std::size_t cell_size = LEAF_CELL_HEADER_SIZE + record.key.size() + record.value.size();
    const std::size_t offset = free_end - cell_size;
    TINYDB_CHECK(storage::PutLittleEndian(bytes, offset + leaf_cell_offset::KEY_BYTES,
                                          static_cast<std::uint16_t>(record.key.size())) &&
                     storage::PutLittleEndian(bytes, offset + leaf_cell_offset::VALUE_BYTES,
                                              static_cast<std::uint16_t>(record.value.size())),
                 "leaf cell header exceeds page");
    bytes[offset + leaf_cell_offset::RESERVED] = std::byte{0};
    std::copy_n(record.key.data(), record.key.size(), page + offset + LEAF_CELL_HEADER_SIZE);
    std::copy_n(record.value.data(), record.value.size(), page + offset + LEAF_CELL_HEADER_SIZE + record.key.size());
    TINYDB_CHECK(storage::PutLittleEndian(bytes, LEAF_HEADER_SIZE + i * SLOT_SIZE, static_cast<slot_t>(offset)),
                 "leaf slot exceeds page");
    free_end = offset;
  }

  TINYDB_CHECK(
      storage::PutLittleEndian(bytes, node_page_offset::CELL_COUNT, static_cast<std::uint16_t>(records_.size())) &&
          storage::PutLittleEndian(bytes, node_page_offset::FREE_START,
                                   static_cast<std::uint16_t>(LEAF_HEADER_SIZE + records_.size() * SLOT_SIZE)) &&
          storage::PutLittleEndian(bytes, node_page_offset::FREE_END, static_cast<std::uint16_t>(free_end)) &&
          storage::PutLittleEndian(bytes, node_page_offset::RESERVED, std::uint16_t{0}) &&
          storage::PutLittleEndian(bytes, node_page_offset::LINK, next_leaf_) && storage::FinalizeDataPage(bytes).Ok(),
      "failed to encode leaf page");
}

auto LeafPageBuilder::Upsert(std::string_view key, std::string_view value) -> bool {
  // The bool means "key is at the right edge", not "new record". Put combines
  // it with the successor sentinel to select the sequential-load split policy.
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  const bool at_tail = it == records_.end();
  if (!at_tail && it->key == key) {
    it->value.assign(value.data(), value.size());
  } else {
    records_.insert(it, Record{std::string(key), std::string(value)});
  }
  return at_tail;
}

auto LeafPageBuilder::Erase(std::string_view key) -> bool {
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  if (it == records_.end() || it->key != key) {
    return false;
  }
  records_.erase(it);
  return true;
}

auto LeafPageBuilder::Bytes() const -> std::size_t {
  std::size_t total = 0;
  for (const auto &record : records_) {
    total += RecordFootprint(record);
  }
  return total;
}

auto LeafPageBuilder::Fits() const -> bool { return Bytes() <= USABLE_BYTES; }

auto LeafPageBuilder::Underfull() const -> bool { return Bytes() < MIN_FILL_BYTES; }

// Choose the legal byte boundary with the smallest encoded-size imbalance.
// Prefix sums make each candidate constant-time to score.
auto LeafPageBuilder::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 2, "too few records to split");

  auto prefix = std::vector<std::size_t>(count + 1, 0);
  for (std::size_t i = 0; i < count; ++i) {
    prefix[i + 1] = prefix[i] + RecordFootprint(records_[i]);
  }

  // Only boundaries where both encoded halves fit are candidates. Among
  // those, prefer the smallest byte imbalance to stabilize occupancy.
  std::size_t best_split = 0;
  std::size_t best_imbalance = 0;
  bool found = false;
  for (std::size_t split = 1; split < count; ++split) {
    const std::size_t left = prefix[split];
    const std::size_t right = prefix[count] - prefix[split];
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
  result.separator = records_[split].key;
  result.right.records_.assign(std::make_move_iterator(split_it), std::make_move_iterator(records_.end()));
  result.right.next_leaf_ = next_leaf_;
  records_.erase(split_it, records_.end());
  next_leaf_ = right_page_id;  // right is inserted between this leaf and old_next
  return result;
}

void LeafPageBuilder::Absorb(LeafPageBuilder &&right) {
  // Adjacent leaf ranges concatenate without sorting. Adopting right's link
  // also removes it from the forward chain. The caller decides merge vs split.
  TINYDB_CHECK(records_.empty() || right.records_.empty() ||
                   txn::BytewiseLess{}(records_.back().key, right.records_.front().key),
               "absorbed leaf does not sort after this one");
  records_.insert(records_.end(), std::make_move_iterator(right.records_.begin()),
                  std::make_move_iterator(right.records_.end()));
  next_leaf_ = right.next_leaf_;
}

}  // namespace tinydb
