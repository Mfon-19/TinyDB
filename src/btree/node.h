#pragma once

#include <tinydb/b_plus_tree.h>
#include <tinydb/check.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/**
  The B+ tree node codec: how sorted records become slotted-page bytes and
  back. This layer knows the on-disk cell formats (declared in b_plus_tree.h)
  but nothing about tree structure.

  Leaf and internal nodes share every routine here via the layout traits
  LeafLayout / InternalLayout: one decoder (LoadNode), one encoder
  (StoreNode), one size model (RecordFootprint / NodeBytes / NodeFits), and
  one split-point chooser (ChooseSplitIndex).
*/

namespace tinydb {

template <typename T>
auto ReadAs(const char *src) -> T {
  static_assert(std::is_trivially_copyable_v<T>);
  T value;
  std::memcpy(&value, src, sizeof(T));
  return value;
}

template <typename T>
void WriteAs(char *dst, const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(dst, &value, sizeof(T));
}

constexpr auto AlignUp(std::size_t value, std::size_t alignment)
    -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

constexpr auto AlignDown(std::size_t value, std::size_t alignment)
    -> std::size_t {
  return value & ~(alignment - 1);
}

using slot_t = std::uint16_t;
constexpr std::size_t SLOT_SIZE = sizeof(slot_t);

// The quarter-page footprint invariant behind split-always-succeeds: with
// every record footprint at most a quarter of a node's usable bytes, the
// split with the smallest byte imbalance is off-balance by at most one
// footprint, so the bigger half stays within (total + usable/4) / 2 <= usable
// for every overflow the tree can build (an insert overflows one page by at
// most one record; underflow repair combines at most half a page, a full
// page, and one separator). A separator key is a leaf key, so
// MAX_ENTRY_BYTES bounds it.
static_assert(SLOT_SIZE + AlignUp(sizeof(LeafCellHeader) + MAX_ENTRY_BYTES,
                                  alignof(LeafCellHeader)) <=
              (PAGE_SIZE - sizeof(LeafHeader)) / 4);
static_assert(SLOT_SIZE + AlignUp(sizeof(InternalCellHeader) + MAX_ENTRY_BYTES,
                                  alignof(InternalCellHeader)) <=
              (PAGE_SIZE - sizeof(InternalHeader)) / 4);

struct LeafRecord {
  std::string key;
  std::string value;
};

struct InternalRecord {
  std::string key;
  page_id_t right_child;
};

struct LeafLayout {
  using Header = LeafHeader;
  using Record = LeafRecord;
  static constexpr NodeType NODE_TYPE = NodeType::Leaf;
  static constexpr std::size_t CELL_ALIGN = alignof(LeafCellHeader);

  static auto GetLink(const Header &header) -> page_id_t {
    return header.next_leaf;
  }
  static void SetLink(Header &header, page_id_t link) {
    header.next_leaf = link;
  }

  static auto CellSize(const Record &record) -> std::size_t {
    return sizeof(LeafCellHeader) + record.key.size() + record.value.size();
  }

  static void WriteCell(char *cell, const Record &record) {
    const auto header = LeafCellHeader{
        .key_size = static_cast<std::uint16_t>(record.key.size()),
        .value_size = static_cast<std::uint16_t>(record.value.size()),
        .flags = 0,
    };
    WriteAs(cell, header);
    std::copy_n(record.key.data(), record.key.size(), cell + sizeof(header));
    std::copy_n(record.value.data(), record.value.size(),
                cell + sizeof(header) + record.key.size());
  }

  static auto ReadCell(const char *page, std::size_t offset)
      -> std::optional<Record> {
    TINYDB_CHECK(offset + sizeof(LeafCellHeader) <= PAGE_SIZE,
                 "leaf cell header overruns page");
    const auto header = ReadAs<LeafCellHeader>(page + offset);
    TINYDB_CHECK(offset + sizeof(header) + header.key_size +
                         header.value_size <=
                     PAGE_SIZE,
                 "leaf cell overruns page");
    if (header.flags != 0) {  // tombstone
      return std::nullopt;
    }
    const char *key = page + offset + sizeof(header);
    return Record{std::string(key, header.key_size),
                  std::string(key + header.key_size, header.value_size)};
  }
};

struct InternalLayout {
  using Header = InternalHeader;
  using Record = InternalRecord;
  static constexpr NodeType NODE_TYPE = NodeType::Internal;
  static constexpr std::size_t CELL_ALIGN = alignof(InternalCellHeader);

  static auto GetLink(const Header &header) -> page_id_t {
    return header.first_child;
  }
  static void SetLink(Header &header, page_id_t link) {
    header.first_child = link;
  }

  static auto CellSize(const Record &record) -> std::size_t {
    return sizeof(InternalCellHeader) + record.key.size();
  }

  static void WriteCell(char *cell, const Record &record) {
    const auto header = InternalCellHeader{
        .right_child = record.right_child,
        .key_size = static_cast<std::uint16_t>(record.key.size()),
    };
    WriteAs(cell, header);
    std::copy_n(record.key.data(), record.key.size(), cell + sizeof(header));
  }

  static auto ReadCell(const char *page, std::size_t offset)
      -> std::optional<Record> {
    TINYDB_CHECK(offset + sizeof(InternalCellHeader) <= PAGE_SIZE,
                 "internal cell header overruns page");
    const auto header = ReadAs<InternalCellHeader>(page + offset);
    TINYDB_CHECK(offset + sizeof(header) + header.key_size <= PAGE_SIZE,
                 "internal cell overruns page");
    return Record{std::string(page + offset + sizeof(header), header.key_size),
                  header.right_child};
  }
};

// A node decoded into sorted records. `link` is next_leaf for leaves and
// first_child for internal nodes.
template <typename Layout>
struct Node {
  page_id_t link{HEADER_PAGE_ID};
  std::vector<typename Layout::Record> records;
};

template <typename Layout>
constexpr std::size_t USABLE_BYTES =
    PAGE_SIZE - sizeof(typename Layout::Header);

// A node whose slots + cells drop below half the usable bytes gets repaired.
template <typename Layout>
constexpr std::size_t MIN_FILL_BYTES = USABLE_BYTES<Layout> / 2;

// Bytes a record consumes in a packed page: its slot plus its aligned cell.
// Exact, because packing always starts from the (aligned) end of the page.
template <typename Layout>
auto RecordFootprint(const typename Layout::Record &record) -> std::size_t {
  return SLOT_SIZE + AlignUp(Layout::CellSize(record), Layout::CELL_ALIGN);
}

template <typename Layout>
auto NodeBytes(const std::vector<typename Layout::Record> &records)
    -> std::size_t {
  std::size_t total = 0;
  for (const auto &record : records) {
    total += RecordFootprint<Layout>(record);
  }
  return total;
}

template <typename Layout>
auto NodeFits(const std::vector<typename Layout::Record> &records) -> bool {
  return NodeBytes<Layout>(records) <= USABLE_BYTES<Layout>;
}

template <typename Layout>
auto LoadNode(const char *page) -> Node<Layout> {
  const auto header = ReadAs<typename Layout::Header>(page);
  TINYDB_CHECK(header.type == Layout::NODE_TYPE,
               "page is not the expected node type");

  Node<Layout> node;
  node.link = Layout::GetLink(header);
  node.records.reserve(header.cell_count);
  const std::size_t slots_end =
      sizeof(typename Layout::Header) + header.cell_count * SLOT_SIZE;

  for (std::size_t i = 0; i < header.cell_count; ++i) {
    const auto offset = static_cast<std::size_t>(
        ReadAs<slot_t>(page + sizeof(typename Layout::Header) + i * SLOT_SIZE));
    TINYDB_CHECK(offset >= slots_end, "cell offset inside header or slots");
    auto record = Layout::ReadCell(page, offset);
    if (!record.has_value()) {
      continue;
    }
    TINYDB_CHECK(node.records.empty() || node.records.back().key < record->key,
                 "keys out of order on page");
    node.records.push_back(std::move(*record));
  }
  return node;
}

// Rewrites the page fully packed: slots ascend after the header, cells grow
// down from the end of the page.
template <typename Layout>
void StoreNode(char *page, const Node<Layout> &node) {
  TINYDB_CHECK(NodeFits<Layout>(node.records), "records do not fit in a page");
  std::memset(page, 0, PAGE_SIZE);

  std::size_t free_end = PAGE_SIZE;
  for (std::size_t i = 0; i < node.records.size(); ++i) {
    const auto &record = node.records[i];
    const std::size_t offset =
        AlignDown(free_end - Layout::CellSize(record), Layout::CELL_ALIGN);
    Layout::WriteCell(page + offset, record);
    WriteAs(page + sizeof(typename Layout::Header) + i * SLOT_SIZE,
            static_cast<slot_t>(offset));
    free_end = offset;
  }

  typename Layout::Header header{};
  header.type = Layout::NODE_TYPE;
  header.cell_count = static_cast<std::uint16_t>(node.records.size());
  header.free_start = static_cast<std::uint16_t>(
      sizeof(typename Layout::Header) + node.records.size() * SLOT_SIZE);
  header.free_end = static_cast<std::uint16_t>(free_end);
  Layout::SetLink(header, node.link);
  WriteAs(page, header);
}

template <typename Records>
auto LowerBoundByKey(Records &records, std::string_view key) {
  return std::lower_bound(records.begin(), records.end(), key,
                          [](const auto &record, std::string_view target) {
                            return record.key < target;
                          });
}

// Split point for an overflowing record sequence: left = [0, s), the record
// at s provides the separator, right starts at s for leaves (the separator
// key is copied up and stays in the right leaf) and at s + 1 for internal
// nodes (the separator moves up; its right_child becomes the right node's
// first_child). Minimizes byte imbalance among split points where both
// halves fit; the entry-size cap guarantees one exists.
template <typename Layout>
auto ChooseSplitIndex(const std::vector<typename Layout::Record> &records)
    -> std::size_t {
  constexpr bool consumes_separator = Layout::NODE_TYPE == NodeType::Internal;
  const std::size_t count = records.size();
  TINYDB_CHECK(count >= (consumes_separator ? 3 : 2),
               "too few records to split");

  auto prefix = std::vector<std::size_t>(count + 1, 0);
  for (std::size_t i = 0; i < count; ++i) {
    prefix[i + 1] = prefix[i] + RecordFootprint<Layout>(records[i]);
  }

  const std::size_t last = consumes_separator ? count - 2 : count - 1;
  std::size_t best_split = 0;
  std::size_t best_imbalance = 0;
  bool found = false;
  for (std::size_t split = 1; split <= last; ++split) {
    const std::size_t left = prefix[split];
    const std::size_t right =
        prefix[count] - prefix[split + (consumes_separator ? 1 : 0)];
    if (left > USABLE_BYTES<Layout> || right > USABLE_BYTES<Layout>) {
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

}  // namespace tinydb
