#pragma once

#include <tinydb/database.h>

namespace tinydb {

/* Benchmark-only endpoint for work initiated by a completed read operation. */
class DatabaseTestAccess final {
 public:
  static void WaitForReadQuiescence(Database &database) noexcept;
};

}  // namespace tinydb
