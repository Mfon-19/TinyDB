#pragma once

#include <tinydb/options.h>
#include <tinydb/status.h>

#include <cstddef>
#include <filesystem>

namespace tinydb::salvage {

struct Report final {
  std::size_t pages_scanned{0};
  std::size_t valid_leaf_pages{0};
  std::size_t rows_recovered{0};
  std::size_t duplicate_rows{0};
  std::size_t damaged_pages{0};
  std::size_t damaged_values{0};
  bool superblock_available{false};
  bool allocator_filter_available{false};
};

/*
** Best-effort recovery is intentionally outside Database::Open.  source is
** opened read-only and destination must not exist.  Locally valid leaf cells
** are copied into a newly encoded database; no source byte is repaired and no
** claim is made that unreachable or transactionally obsolete rows represent
** the last committed logical state.
*/
auto Run(const std::filesystem::path &source, const std::filesystem::path &destination,
         Options destination_options = {}) -> Result<Report>;

}  // namespace tinydb::salvage
