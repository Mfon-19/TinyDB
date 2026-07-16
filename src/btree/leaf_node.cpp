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

#include "leaf_node.h"
#include "page_format.h"
#include "storage/encoding.h"
#include "storage/page_codec.h"
#include "txn/contract.h"

/*
  LeafNode is the logical codec for one leaf page: Load decodes the packed
  on-disk format (drawn in page_format.h) into an editable in-memory shape

      records   = [(key, value), ...] sorted by key, keys unique
      next_leaf = page id of the next leaf, or HEADER_PAGE_ID at the tail

  and Store re-encodes it. Leaves hold every value in the tree — internal
  pages only route — and range scans walk the next_leaf chain:

      leaf A              leaf B              leaf C
    [a b c]  --next-->  [k m n]  --next-->  [x y z]

  Invariants maintained here:

  - records_ stays sorted with unique keys through every mutation. Load
    re-verifies node type, cell bounds, and key order, so a corrupt page
    aborts at decode time instead of silently misrouting searches.

  - Store writes only fully packed pages and aborts unless Fits() holds:
    the caller proves the fit (splitting first if it must) before anything
    reaches the page. Every write rebuilds slots and cells from scratch, so
    fragmentation never accumulates.

  What the tree above relies on:

  - Split always yields two halves that fit in a page. That holds because
    entries are capped at MAX_ENTRY_BYTES; ChooseSplitIndex aborts if the
    cap was violated rather than let an overfull half be written.

  - The split separator is a copy of the right half's first key: leaf keys
    never leave the leaf level (unlike internal splits, which move the
    separator up and out of the node).
*/

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - LEAF_HEADER_SIZE;

// The underfull threshold: a node whose records pack into less than half
// the usable page triggers a repair after a delete. Half is the classic
// B+ tree minimum, and it is not arbitrary — two nodes that are both below
// half full always fit back into a single page, so a merge can never fail
// for space.
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES / 2;

auto KeyIsBefore(const LeafNode::Record &record, std::string_view target) -> bool {
  return txn::BytewiseLess{}(record.key, target);
}

// The bytes one record will consume in a packed page: its slot plus its
// cell, the cell rounded up to the cell header's alignment. This is exact,
// not an estimate — Store packs cells contiguously from the (aligned) end
// of the page, so footprints simply add, and Fits() can prove a layout
// will succeed without attempting it.
auto RecordFootprint(const LeafNode::Record &record) -> std::size_t {
  return SLOT_SIZE + LEAF_CELL_HEADER_SIZE + record.key.size() + record.value.size();
}

}  // namespace

// Page bytes -> sorted records. Every structural claim the page makes —
// node type, slot count, cell offsets, cell sizes, key order — is checked
// before it is used, so a corrupt page aborts here with a message naming
// what broke, instead of leaking garbage records into the tree above.
auto LeafNode::Load(const char *page) -> LeafNode {
  const auto bytes = std::as_bytes(std::span{page, PAGE_SIZE});
  TINYDB_CHECK(RawNodeType(page) == static_cast<std::uint16_t>(NodeType::Leaf), "page is not a leaf node");
  const auto cell_count = storage::GetLittleEndian<std::uint16_t>(bytes, node_page_offset::CELL_COUNT).value();

  LeafNode node;
  node.next_leaf_ = storage::GetLittleEndian<page_id_t>(bytes, node_page_offset::LINK).value();
  node.records_.reserve(cell_count);
  const std::size_t slots_end = LEAF_HEADER_SIZE + cell_count * SLOT_SIZE;
  TINYDB_CHECK(slots_end <= PAGE_SIZE, "slot array overruns page");

  for (std::size_t i = 0; i < cell_count; ++i) {
    const auto offset =
        static_cast<std::size_t>(storage::GetLittleEndian<slot_t>(bytes, LEAF_HEADER_SIZE + i * SLOT_SIZE).value());
    TINYDB_CHECK(offset >= slots_end, "cell offset inside header or slots");
    TINYDB_CHECK(offset + LEAF_CELL_HEADER_SIZE <= PAGE_SIZE, "leaf cell header overruns page");
    const auto key_size = storage::GetLittleEndian<std::uint16_t>(bytes, offset + leaf_cell_offset::KEY_BYTES).value();
    const auto value_size =
        storage::GetLittleEndian<std::uint16_t>(bytes, offset + leaf_cell_offset::VALUE_BYTES).value();
    TINYDB_CHECK(offset + LEAF_CELL_HEADER_SIZE + key_size + value_size <= PAGE_SIZE, "leaf cell overruns page");
    TINYDB_CHECK(bytes[offset + leaf_cell_offset::RESERVED] == std::byte{0}, "leaf cell reserved byte is nonzero");
    const char *key = page + offset + LEAF_CELL_HEADER_SIZE;
    auto record = Record{std::string(key, key_size), std::string(key + key_size, value_size)};
    TINYDB_CHECK(node.records_.empty() || txn::BytewiseLess{}(node.records_.back().key, record.key),
                 "keys out of order on page");
    node.records_.push_back(std::move(record));
  }
  return node;
}

// Records -> page bytes, rebuilt from scratch (memset first, so no stale
// bytes from the page's previous life survive). Cells are placed from the
// end of the page downward, each offset aligned down so the cell header
// can be read back in place; the slot array ascends right after the page
// header in record order, which is key order; the header itself is written
// last, once the final free-space bounds are known. Callers must have
// proven Fits() — an overfull store is a logic bug upstream, not an I/O
// problem, so it aborts.
void LeafNode::Store(char *page, page_id_t page_id) const {
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

auto LeafNode::Upsert(std::string_view key, std::string_view value) -> bool {
  /*
    Insert key or replace its value, keeping records_ sorted either way.

    The return value is not "inserted vs updated" — it answers "did this
    key land past everything already here?" The tree combines that answer
    with "this is the rightmost leaf" (next_leaf == HEADER_PAGE_ID) to
    recognize an ascending bulk load in progress and switch to the
    tail-heavy split; see Split for why that matters.
  */
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  const bool at_tail = it == records_.end();
  if (!at_tail && it->key == key) {
    it->value.assign(value.data(), value.size());
  } else {
    records_.insert(it, Record{std::string(key), std::string(value)});
  }
  return at_tail;
}

auto LeafNode::Erase(std::string_view key) -> bool {
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  if (it == records_.end() || it->key != key) {
    return false;
  }
  records_.erase(it);
  return true;
}

auto LeafNode::Get(std::string_view key) const -> std::optional<std::string> {
  const auto it = std::lower_bound(records_.begin(), records_.end(), key, KeyIsBefore);
  if (it != records_.end() && it->key == key) {
    return it->value;
  }
  return std::nullopt;
}

auto LeafNode::Bytes() const -> std::size_t {
  std::size_t total = 0;
  for (const auto &record : records_) {
    total += RecordFootprint(record);
  }
  return total;
}

auto LeafNode::Fits() const -> bool { return Bytes() <= USABLE_BYTES; }

auto LeafNode::Underfull() const -> bool { return Bytes() < MIN_FILL_BYTES; }

// Picks the split point s for an overflowing leaf: left = [0, s) and
// right = [s, end), with the key at s copied up as the separator. Among
// the split points where both halves actually fit in a page, it takes the
// one with the smallest byte imbalance — balanced halves leave both pages
// the most room to grow before splitting again. Records vary in size, so
// this is a byte decision, not a count decision: prefix sums of the
// footprints let every candidate be scored in one pass. At least one
// candidate always exists because entries are capped at MAX_ENTRY_BYTES
// (see the static_asserts in page_format.h); if none does, the cap was
// violated upstream and this aborts rather than write an overfull page.
auto LeafNode::ChooseSplitIndex() const -> std::size_t {
  const std::size_t count = records_.size();
  TINYDB_CHECK(count >= 2, "too few records to split");

  auto prefix = std::vector<std::size_t>(count + 1, 0);
  for (std::size_t i = 0; i < count; ++i) {
    prefix[i + 1] = prefix[i] + RecordFootprint(records_[i]);
  }

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

auto LeafNode::Split(page_id_t right_page_id, bool tail_heavy) -> SplitResult {
  /*
    Splits this overflowing leaf in two: this node keeps the left half,
    the returned node takes the right half, and the chain is pre-wired so
    the caller only has to write both pages out:

        before:
          this:  [a b c k m z] -> old_next

        after:
          this:  [a b c] -> right_page_id
          right: [k m z] -> old_next

        separator: "k" — the right half's first key

    The separator is a copy, not a move: "k" stays in the right leaf,
    because leaves must hold every value, and the parent stores its own
    copy purely for routing. (Internal splits are the opposite — see
    InternalNode::Split — since internal keys are routing-only, the
    promoted key need not be kept below.)

    The tail-heavy mode serves ascending bulk loads. When the caller saw
    the new key land at the very tail of the rightmost leaf, an even split
    would leave a trail of half-full pages behind the load; cleaving off
    just the new record leaves dense pages instead:

        [old old old old new]  ->  [old old old old] -> [new]

    Ascending loads are common enough (imported data, time-ordered keys)
    that this one branch roughly doubles their leaf occupancy.
  */
  const bool lopsided = tail_heavy && records_.size() >= 2;
  const std::size_t split = lopsided ? records_.size() - 1 : ChooseSplitIndex();
  const auto split_it = records_.begin() + static_cast<std::ptrdiff_t>(split);

  SplitResult result;
  result.separator = records_[split].key;
  result.right.records_.assign(std::make_move_iterator(split_it), std::make_move_iterator(records_.end()));
  result.right.next_leaf_ = next_leaf_;
  records_.erase(split_it, records_.end());
  next_leaf_ = right_page_id;  // splice the new right leaf into the chain
  return result;
}

void LeafNode::Absorb(LeafNode &&right) {
  /*
    Merges the right sibling into this node. The two are adjacent leaves,
    so every key in right sorts after every key here, and the merge is
    just an append — plus adopting right's next pointer, which is what
    unlinks right from the chain:

        before:
          this:  [a b c] -> right
          right: [k m z] -> old_next

        after:
          this:  [a b c k m z] -> old_next

    The result may well not Fit(); the caller checks, and either keeps the
    merge (it fit) or re-splits the combined node as a rebalance. Either
    way the caller also finishes the tree-level bookkeeping: fixing the
    parent's separator, and freeing right's page if it merged away.
  */
  TINYDB_CHECK(records_.empty() || right.records_.empty() ||
                   txn::BytewiseLess{}(records_.back().key, right.records_.front().key),
               "absorbed leaf does not sort after this one");
  records_.insert(records_.end(), std::make_move_iterator(right.records_.begin()),
                  std::make_move_iterator(right.records_.end()));
  next_leaf_ = right.next_leaf_;
}

}  // namespace tinydb
