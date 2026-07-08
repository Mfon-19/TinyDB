#include <tinydb/check.h>
#include <tinydb/page.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "internal_node.h"
#include "page_format.h"

namespace tinydb {
namespace {

/*
  InternalNode is the logical codec for one internal page. The tree code
  works with this in-memory shape:

      first_child = C0
      records     = [K0 -> C1, K1 -> C2, K2 -> C3]

  That represents this routing table:

      key < K0        -> C0
      K0 <= key < K1  -> C1
      K1 <= key < K2  -> C2
      K2 <= key       -> C3

  On disk, first_child lives in the header and each record is a slotted cell
  containing {right_child, key_size, key bytes}. Load verifies type, cell
  bounds, and key ordering; Store rewrites a fully packed page from records_.
*/

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - sizeof(InternalHeader);

// A node whose slots + cells drop below half the usable bytes gets repaired.
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES / 2;

auto KeyIsBefore(const InternalNode::Record &record, std::string_view target) -> bool { return record.key < target; }

// Bytes a record consumes in a packed page: its slot plus its aligned cell.
// Exact, because packing always starts from the (aligned) end of the page.
auto RecordFootprint(const InternalNode::Record &record) -> std::size_t {
  const std::size_t cell_size = sizeof(InternalCellHeader) + record.key.size();
  return SLOT_SIZE + AlignUp(cell_size, alignof(InternalCellHeader));
}

}  // namespace

InternalNode::InternalNode(page_id_t first_child, std::string separator, page_id_t right_child)
    : first_child_(first_child) {
  records_.push_back(Record{std::move(separator), right_child});
}

auto InternalNode::Load(const char *page) -> InternalNode {
  const auto header = ReadAs<InternalHeader>(page);
  TINYDB_CHECK(header.type == NodeType::Internal, "page is not an internal node");

  InternalNode node;
  node.first_child_ = header.first_child;
  node.records_.reserve(header.cell_count);
  const std::size_t slots_end = sizeof(InternalHeader) + header.cell_count * SLOT_SIZE;
  TINYDB_CHECK(slots_end <= PAGE_SIZE, "slot array overruns page");

  for (std::size_t i = 0; i < header.cell_count; ++i) {
    const auto offset = static_cast<std::size_t>(ReadAs<slot_t>(page + sizeof(InternalHeader) + i * SLOT_SIZE));
    TINYDB_CHECK(offset >= slots_end, "cell offset inside header or slots");
    TINYDB_CHECK(offset + sizeof(InternalCellHeader) <= PAGE_SIZE, "internal cell header overruns page");
    const auto cell = ReadAs<InternalCellHeader>(page + offset);
    TINYDB_CHECK(offset + sizeof(cell) + cell.key_size <= PAGE_SIZE, "internal cell overruns page");
    auto record = Record{std::string(page + offset + sizeof(cell), cell.key_size), cell.right_child};
    TINYDB_CHECK(node.records_.empty() || node.records_.back().key < record.key, "keys out of order on page");
    node.records_.push_back(std::move(record));
  }
  return node;
}

void InternalNode::Store(char *page) const {
  TINYDB_CHECK(Fits(), "records do not fit in a page");
  std::memset(page, 0, PAGE_SIZE);

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const std::size_t cell_size = sizeof(InternalCellHeader) + record.key.size();
    const std::size_t offset = AlignDown(free_end - cell_size, alignof(InternalCellHeader));
    const auto cell = InternalCellHeader{
        .right_child = record.right_child,
        .key_size = static_cast<std::uint16_t>(record.key.size()),
    };
    WriteAs(page + offset, cell);
    std::copy_n(record.key.data(), record.key.size(), page + offset + sizeof(cell));
    WriteAs(page + sizeof(InternalHeader) + i * SLOT_SIZE, static_cast<slot_t>(offset));
    free_end = offset;
  }

  auto header = InternalHeader{};
  header.type = NodeType::Internal;
  header.cell_count = static_cast<std::uint16_t>(records_.size());
  header.free_start = static_cast<std::uint16_t>(sizeof(InternalHeader) + records_.size() * SLOT_SIZE);
  header.free_end = static_cast<std::uint16_t>(free_end);
  header.first_child = first_child_;
  WriteAs(page, header);
}

auto InternalNode::FindChildIndex(std::string_view key) const -> std::size_t {
  /*
    upper_bound implements the "equal goes right" separator rule.

      records: [K0, K1, K2]

        key < K0       -> index 0 -> first_child
        key == K0      -> index 1 -> K0.right_child
        K0 < key < K1  -> index 1 -> K0.right_child
        key >= K2      -> index 3 -> K2.right_child
  */
  const auto it = std::upper_bound(records_.begin(), records_.end(), key,
                                   [](std::string_view target, const Record &record) { return target < record.key; });
  return static_cast<std::size_t>(it - records_.begin());
}

auto InternalNode::ChildAt(std::size_t child_index) const -> page_id_t {
  TINYDB_CHECK(child_index <= records_.size(), "child index out of range");
  return child_index == 0 ? first_child_ : records_[child_index - 1].right_child;
}

auto InternalNode::SeparatorKeyAt(std::size_t index) const -> const std::string & {
  TINYDB_CHECK(index < records_.size(), "separator index out of range");
  return records_[index].key;
}

void InternalNode::SetSeparatorKey(std::size_t index, std::string key) {
  TINYDB_CHECK(index < records_.size(), "separator index out of range");
  records_[index].key = std::move(key);
}

void InternalNode::InsertSeparator(std::string key, page_id_t right_child) {
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  records_.insert(it, Record{std::move(key), right_child});
}

void InternalNode::EraseSeparator(std::size_t index) {
  TINYDB_CHECK(index < records_.size(), "separator index out of range");
  records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(index));
}

auto InternalNode::Bytes() const -> std::size_t {
  std::size_t total = 0;
  for (const auto &record : records_) {
    total += RecordFootprint(record);
  }
  return total;
}

auto InternalNode::Fits() const -> bool { return Bytes() <= USABLE_BYTES; }

auto InternalNode::Underfull() const -> bool { return Bytes() < MIN_FILL_BYTES; }

// Split point for an overflowing internal node: left = [0, s), the separator
// at s moves up to the parent, right = [s + 1, end). Minimizes byte
// imbalance among split points where both halves fit; the entry-size cap
// guarantees one exists.
auto InternalNode::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 3, "too few records to split");

  auto prefix = std::vector<std::size_t>(count + 1, 0);
  for (std::size_t i = 0; i < count; ++i) {
    prefix[i + 1] = prefix[i] + RecordFootprint(records_[i]);
  }

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

auto InternalNode::Split() -> SplitResult {
  /*
    Splitting an internal node promotes the middle separator. Unlike a leaf
    split, the separator does not remain in either child.

      before:
        first=C0
        records=[K0->C1, K1->C2, K2->C3, K3->C4]

      split at K2:

        this(left):
          first=C0
          records=[K0->C1, K1->C2]

        result.separator:
          K2

        result.right:
          first=C3
          records=[K3->C4]

    The promoted separator's right_child becomes the first_child of the
    returned right node. That is what preserves the N separators / N+1
    children invariant after K2 moves up.
  */
  const std::size_t split = ChooseSplitIndex();
  const auto split_it = records_.begin() + static_cast<std::ptrdiff_t>(split);

  SplitResult result;
  result.separator = std::move(records_[split].key);
  result.right.first_child_ = records_[split].right_child;
  result.right.records_.assign(std::make_move_iterator(split_it + 1), std::make_move_iterator(records_.end()));
  records_.erase(split_it, records_.end());
  return result;
}

void InternalNode::Absorb(std::string separator, InternalNode &&right) {
  /*
    Merge two adjacent internal nodes by pulling the parent separator down
    between them.

      parent had:
                  separator S
                 /           \
             this(left)      right

      this before:
        first=C0, records=[K0->C1]

      right before:
        first=C2, records=[K2->C3]

      this after Absorb(S, right):
        first=C0, records=[K0->C1, S->C2, K2->C3]

    The separator points at right.first_child because that child owns keys
    >= S and below right's first separator.
  */
  TINYDB_CHECK(records_.empty() || records_.back().key < separator, "separator does not sort after this node's keys");
  records_.push_back(Record{std::move(separator), right.first_child_});
  records_.insert(records_.end(), std::make_move_iterator(right.records_.begin()),
                  std::make_move_iterator(right.records_.end()));
}

}  // namespace tinydb
