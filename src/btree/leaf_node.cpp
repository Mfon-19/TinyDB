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

namespace tinydb {
namespace {

constexpr std::size_t USABLE_BYTES = PAGE_SIZE - sizeof(LeafHeader);

// A node whose slots + cells drop below half the usable bytes gets repaired.
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES / 2;

auto KeyIsBefore(const LeafNode::Record &record, std::string_view target) -> bool { return record.key < target; }

// Bytes a record consumes in a packed page: its slot plus its aligned cell.
// Exact, because packing always starts from the (aligned) end of the page.
auto RecordFootprint(const LeafNode::Record &record) -> std::size_t {
  const std::size_t cell_size = sizeof(LeafCellHeader) + record.key.size() + record.value.size();
  return SLOT_SIZE + AlignUp(cell_size, alignof(LeafCellHeader));
}

}  // namespace

auto LeafNode::Load(const char *page) -> LeafNode {
  const auto header = ReadAs<LeafHeader>(page);
  TINYDB_CHECK(header.type == NodeType::Leaf, "page is not a leaf node");

  LeafNode node;
  node.next_leaf_ = header.next_leaf;
  node.records_.reserve(header.cell_count);
  const std::size_t slots_end = sizeof(LeafHeader) + header.cell_count * SLOT_SIZE;

  for (std::size_t i = 0; i < header.cell_count; ++i) {
    const auto offset = static_cast<std::size_t>(ReadAs<slot_t>(page + sizeof(LeafHeader) + i * SLOT_SIZE));
    TINYDB_CHECK(offset >= slots_end, "cell offset inside header or slots");
    TINYDB_CHECK(offset + sizeof(LeafCellHeader) <= PAGE_SIZE, "leaf cell header overruns page");
    const auto cell = ReadAs<LeafCellHeader>(page + offset);
    TINYDB_CHECK(offset + sizeof(cell) + cell.key_size + cell.value_size <= PAGE_SIZE, "leaf cell overruns page");
    if (cell.flags != 0) {  // tombstone
      continue;
    }
    const char *key = page + offset + sizeof(cell);
    auto record = Record{std::string(key, cell.key_size), std::string(key + cell.key_size, cell.value_size)};
    TINYDB_CHECK(node.records_.empty() || node.records_.back().key < record.key, "keys out of order on page");
    node.records_.push_back(std::move(record));
  }
  return node;
}

void LeafNode::Store(char *page) const {
  TINYDB_CHECK(Fits(), "records do not fit in a page");
  std::memset(page, 0, PAGE_SIZE);

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < records_.size(); ++i) {
    const auto &record = records_[i];
    const std::size_t cell_size = sizeof(LeafCellHeader) + record.key.size() + record.value.size();
    const std::size_t offset = AlignDown(free_end - cell_size, alignof(LeafCellHeader));
    const auto cell = LeafCellHeader{
        .key_size = static_cast<std::uint16_t>(record.key.size()),
        .value_size = static_cast<std::uint16_t>(record.value.size()),
        .flags = 0,
    };
    WriteAs(page + offset, cell);
    std::copy_n(record.key.data(), record.key.size(), page + offset + sizeof(cell));
    std::copy_n(record.value.data(), record.value.size(), page + offset + sizeof(cell) + record.key.size());
    WriteAs(page + sizeof(LeafHeader) + i * SLOT_SIZE, static_cast<slot_t>(offset));
    free_end = offset;
  }

  auto header = LeafHeader{};
  header.type = NodeType::Leaf;
  header.cell_count = static_cast<std::uint16_t>(records_.size());
  header.free_start = static_cast<std::uint16_t>(sizeof(LeafHeader) + records_.size() * SLOT_SIZE);
  header.free_end = static_cast<std::uint16_t>(free_end);
  header.next_leaf = next_leaf_;
  WriteAs(page, header);
}

auto LeafNode::Upsert(std::string_view key, std::string_view value) -> bool {
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

// Split point for an overflowing leaf: left = [0, s), right = [s, end); the
// key at s is copied up as the separator and stays in the right leaf.
// Minimizes byte imbalance among split points where both halves fit; the
// entry-size cap guarantees one exists.
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
  TINYDB_CHECK(records_.empty() || right.records_.empty() || records_.back().key < right.records_.front().key,
               "absorbed leaf does not sort after this one");
  records_.insert(records_.end(), std::make_move_iterator(right.records_.begin()),
                  std::make_move_iterator(right.records_.end()));
  next_leaf_ = right.next_leaf_;
}

}  // namespace tinydb
