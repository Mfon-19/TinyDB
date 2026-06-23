#include "synarch/database.h"

#include <utility>

namespace synarch {

Database::Database(std::filesystem::path path) : path_(std::move(path)), open_(true) {}

Result<Database> Database::open(const std::filesystem::path& path) {
  if (path.empty()) {
    return Result<Database>::failure(Status::invalid_argument("database path must not be empty"));
  }

  return Result<Database>::success(Database(path));
}

Status Database::close() {
  open_ = false;
  return Status::ok();
}

Result<ResultSet> Database::execute(std::string_view sql) const {
  if (!open_) {
    return Result<ResultSet>::failure(Status::internal("database is closed"));
  }

  if (sql.empty()) {
    return Result<ResultSet>::failure(Status::invalid_argument("SQL must not be empty"));
  }

  return Result<ResultSet>::success(ResultSet{});
}

const std::filesystem::path& Database::path() const noexcept { return path_; }

}  // namespace synarch
