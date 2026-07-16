#pragma once

#include <tinydb/page.h>

#include <cstdint>

namespace tinydb::txn {

// One immutable description of the committed database. Readers capture this
// value together with the committed page table, so every lookup in a snapshot
// starts from the same roots and durability frontier.
struct DatabaseState final {
  page_id_t root_page_id{HEADER_PAGE_ID};
  page_id_t allocator_root_page_id{HEADER_PAGE_ID};
  page_id_t high_water_page_id{FIRST_DATA_PAGE_ID};
  std::uint64_t transaction_id{0};
  std::uint64_t visible_lsn{0};
  std::uint64_t checkpoint_lsn{0};

  auto operator==(const DatabaseState &) const -> bool = default;
};

}  // namespace tinydb::txn
