#pragma once

#include <tinydb/database.h>

#include <cstdint>

namespace tinydb {

struct ReadAheadCounters final {
  std::uint64_t plans{0};
  std::uint64_t pages_scheduled{0};
  std::uint64_t pages_consumed{0};
};

class DatabaseTestAccess final {
 public:
  static void WaitForReadQuiescence(Database &database) noexcept;
  static auto ReadAhead(Database &database) noexcept -> ReadAheadCounters;
};

}  // namespace tinydb
