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
#include "storage/encoding.h"
#include "storage/page_codec.h"

/*
  InternalNode is the logical codec for one internal page: Load decodes the
  packed on-disk format (drawn in page_format.h) into an editable routing
  table

      first_child = C0
      records     = [K0 -> C1, K1 -> C2, K2 -> C3]

  meaning

      key < K0        -> C0
      K0 <= key < K1  -> C1
      K1 <= key < K2  -> C2
      K2 <= key       -> C3

  and Store re-encodes it. On disk, first_child lives in the page header and
  each record is a slotted cell of {right_child, key_size, key bytes}.

  Invariants maintained here:

  - records_ stays sorted with unique separator keys through every
    mutation, and a node with N separators always has N + 1 children (the
    extra child is first_child_). Load re-verifies node type, cell bounds,
    and key order, so a corrupt page aborts at decode time instead of
    silently misrouting searches.

  - Store writes only fully packed pages and aborts unless Fits() holds:
    the caller proves the fit (splitting first if it must) before anything
    reaches the page. Every write rebuilds slots and cells from scratch, so
    fragmentation never accumulates.

  What the tree above relies on:

  - Split promotes the middle separator up and out of the node (leaf splits
    copy their separator; internal splits surrender it) and hands the
    promoted separator's right child to the new right half as its
    first_child — that is what preserves N separators / N + 1 children on
    both sides.

  - Split always yields two halves that fit in a page; ChooseSplitIndex
    aborts if the MAX_ENTRY_BYTES cap was violated rather than let an
    overfull half be written.
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - INTERNAL_HEADER_SIZE;

// The underfull threshold: a node whose records pack into less than half
// the usable page triggers a repair after a delete. Half is the classic
// B+ tree minimum, and it is not arbitrary — two nodes that are both below
// half full always fit back into a single page, so a merge can never fail
// for space. (For internal nodes the pulled-down parent separator rides
// along in the merge, which can push the pair over; the repair handles
// that by re-splitting.)
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES / 2;

auto KeyIsBefore(const InternalNode::Record &record, std::string_view target) -> bool { return record.key < target; }

// The bytes one record will consume in a packed page: its slot plus its
// cell, the cell rounded up to the cell header's alignment. This is exact,
// not an estimate — Store packs cells contiguously from the (aligned) end
// of the page, so footprints simply add, and Fits() can prove a layout
// will succeed without attempting it.
auto RecordFootprint(const InternalNode::Record &record) -> std::size_t {
  return SLOT_SIZE + INTERNAL_CELL_HEADER_SIZE + record.key.size();
}

}  // namespace

// The two-child root built when a root split grows the tree a level: the
// left half on first_child, one separator routing to the right half.
InternalNode::InternalNode(page_id_t first_child, std::string separator, page_id_t right_child)
    : first_child_(first_child) {
  records_.push_back(Record{std::move(separator), right_child});
}

// Page bytes -> routing table. Every structural claim the page makes —
// node type, slot count, cell offsets, cell sizes, key order — is checked
// before it is used, so a corrupt page aborts here with a message naming
// what broke, instead of misrouting every search that passes through it.
auto InternalNode::Load(const char *page) -> InternalNode {
  const auto bytes = std::as_bytes(std::span{page, PAGE_SIZE});
  TINYDB_CHECK(RawNodeType(page) == static_cast<std::uint16_t>(NodeType::Internal), "page is not an internal node");
  const auto cell_count = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::CELL_COUNT).value();

  InternalNode node;
  node.first_child_ = storage::GetLittleEndian<page_id_t>(bytes, node_page_offset::LINK).value();
  node.records_.reserve(cell_count);
  const std::size_t slots_end = INTERNAL_HEADER_SIZE + cell_count * SLOT_SIZE;
  TINYDB_CHECK(slots_end <= PAGE_SIZE, "slot array overruns page");

  for (std::size_t i = 0; i < cell_count; ++i) {
    const auto offset =
        static_cast<std::size_t>(storage::GetLittleEndian<slot_t>(bytes, INTERNAL_HEADER_SIZE + i * SLOT_SIZE).value());
    TINYDB_CHECK(offset >= slots_end, "cell offset inside header or slots");
    TINYDB_CHECK(offset + INTERNAL_CELL_HEADER_SIZE <= PAGE_SIZE, "internal cell header overruns page");
    const auto right_child =
        storage::GetLittleEndian<page_id_t>(bytes, offset + internal_cell_offset::RIGHT_CHILD).value();
    const auto key_size =
        storage::GetLittleEndian<std::uint16_t>(bytes, offset + internal_cell_offset::KEY_BYTES).value();
    TINYDB_CHECK(offset + INTERNAL_CELL_HEADER_SIZE + key_size <= PAGE_SIZE, "internal cell overruns page");
    auto record = Record{std::string(page + offset + INTERNAL_CELL_HEADER_SIZE, key_size), right_child};
    TINYDB_CHECK(node.records_.empty() || node.records_.back().key < record.key, "keys out of order on page");
    node.records_.push_back(std::move(record));
  }
  return node;
}

// Routing table -> page bytes, rebuilt from scratch (memset first, so no
// stale bytes from the page's previous life survive). Cells are placed
// from the end of the page downward, each offset aligned down so the cell
// header can be read back in place; the slot array ascends right after the
// page header in record order, which is key order; the header itself —
// including first_child — is written last, once the final free-space
// bounds are known. Callers must have proven Fits(): an overfull store is
// a logic bug upstream, not an I/O problem, so it aborts.
void InternalNode::Store(char *page, page_id_t page_id) const {
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

auto InternalNode::FindChildIndex(std::string_view key) const -> std::size_t {
  /*
    upper_bound finds the first separator strictly greater than key; the
    child just before that boundary is the one that owns key. This is the
    "equal goes right" rule — a key equal to a separator belongs to the
    separator's right child:

        records: [K0, K1, K2]

          key < K0       -> index 0 -> first_child
          key == K0      -> index 1 -> K0.right_child
          K0 < key < K1  -> index 1 -> K0.right_child
          key >= K2      -> index 3 -> K2.right_child

    "Equal goes right" is forced by how leaf splits make separators: the
    separator is a copy of the right half's first key, so the key equal to
    it lives in the right half. lower_bound here would send lookups for
    that exact key down the wrong side.
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

// Picks the split point s for an overflowing internal node: left = [0, s),
// right = [s + 1, end), and the record at s in neither half — its key is
// promoted to the parent. That excluded middle is why this needs at least
// three records where the leaf version needs two, and why the right half's
// bytes are summed from s + 1. Among the split points where both halves
// fit in a page, it takes the one with the smallest byte imbalance,
// scoring every candidate in one pass over prefix sums of the footprints.
// At least one candidate always exists because entries are capped at
// MAX_ENTRY_BYTES (see the static_asserts in page_format.h); if none does,
// the cap was violated upstream and this aborts rather than write an
// overfull page.
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
    Splits this overflowing node in two: this node keeps the left half,
    the middle separator is promoted out entirely, and the returned node
    takes the right half.

        before:
          first=C0
          records=[K0->C1, K1->C2, K2->C3, K3->C4]

        split at K2:

          this (left):
            first=C0
            records=[K0->C1, K1->C2]

          result.separator:
            K2

          result.right:
            first=C3
            records=[K3->C4]

    Unlike a leaf split, K2 stays in neither half. Internal keys are pure
    routing, so once the parent stores K2 a copy below would be a wasted
    slot — but K2's right child C3 must survive, and it becomes the right
    half's first_child. Count the children to see why that handoff is
    forced: the node had 5 children for 4 separators; afterward the left
    has 3 children for 2 separators and the right needs 2 children for its
    1 separator, and C3 — no longer reachable through K2 — is exactly the
    missing one.
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
    Merges the right sibling into this node, with the parent's separator
    pulled down between them. Leaves can merge by plain concatenation, but
    internal nodes cannot: the keys under right's first child fall between
    this node's last separator and right's first one, and the only key
    that marks that boundary is the separator the parent kept. So it comes
    down and becomes a real record here:

        parent had:      separator S
                        /           \
              this (left)           right

        this before:   first=C0, records=[K0->C1]
        right before:  first=C2, records=[K2->C3]

        after Absorb(S, right):
          first=C0, records=[K0->C1, S->C2, K2->C3]

    S takes right's old first_child as its right child — exactly the
    subtree holding keys >= S and below right's first separator. This is
    the mirror image of Split, where the promoted separator donates its
    right child to become the new right half's first_child.

    The result may not Fit(); the caller checks, and either keeps the
    merge or re-splits the combined node as a rebalance. The caller also
    erases S from the parent (merge) or replaces it (rebalance), and frees
    right's page if it merged away.
  */
  TINYDB_CHECK(records_.empty() || records_.back().key < separator, "separator does not sort after this node's keys");
  records_.push_back(Record{std::move(separator), right.first_child_});
  records_.insert(records_.end(), std::make_move_iterator(right.records_.begin()),
                  std::make_move_iterator(right.records_.end()));
}

}  // namespace tinydb
