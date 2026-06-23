#pragma once

#include <filesystem>
#include <string_view>

#include "synarch/result.h"
#include "synarch/status.h"

namespace synarch {

class Database {
public:
  Database(const Database&) = delete;
  auto operator=(const Database&) -> Database& = delete;
  Database(Database&&) noexcept = default;
  auto operator=(Database&&) noexcept -> Database& = default;
  ~Database() = default;

  [[nodiscard]] static Result<Database> open(const std::filesystem::path& path);

  [[nodiscard]] Status close();

  [[nodiscard]] Result<ResultSet> execute(std::string_view sql) const;

  [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
  explicit Database(std::filesystem::path path);

  std::filesystem::path path_;
  bool open_{false};
};

}  // namespace synarch
