/*
 * This is the API to the database that callers use
 */

#include "tinydb/status.h"
#include <string_view>

namespace tinydb {

class Database {
public:
  static Status Open(const std::string_view name) {
    return Status::Ok();
  }

  Database() = default;

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
};
} // namespace tinydb