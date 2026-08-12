#pragma once

#include <tinydb/database.h>

#include "io/testable_posix.h"
#include "wal/wal.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb::test {

class ScopedTestHook {
 public:
  explicit ScopedTestHook(io::TestHook hook) { io::SetTestHook(std::move(hook)); }
  ~ScopedTestHook() { io::ClearTestHook(); }

  ScopedTestHook(const ScopedTestHook &) = delete;
  auto operator=(const ScopedTestHook &) -> ScopedTestHook & = delete;
};

inline auto Path(std::string_view name) -> std::filesystem::path {
  const auto *const configured_root = std::getenv("TINYDB_TEST_ROOT");
  const auto root = configured_root == nullptr || *configured_root == '\0' ? std::filesystem::temp_directory_path()
                                                                           : std::filesystem::path{configured_root};
  return root / ("tinydb_test_" + std::string(name) + "_" + std::to_string(::getpid()) + ".db");
}

inline void Remove(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(Wal::PathFor(path), ignored);
}

inline void Copy(const std::filesystem::path &from, const std::filesystem::path &to) {
  Remove(to);
  std::filesystem::copy_file(from, to);
  const auto source_wal = Wal::PathFor(from);
  if (std::filesystem::exists(source_wal)) {
    std::filesystem::copy_file(source_wal, Wal::PathFor(to));
  }
}

inline auto ReadFile(const std::filesystem::path &path) -> std::string {
  auto input = std::ifstream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void WriteFile(const std::filesystem::path &path, std::string_view bytes) {
  auto output = std::ofstream(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline auto Key(std::size_t index) -> std::string {
  auto key = std::to_string(index);
  key.insert(key.begin(), 8U - key.size(), '0');
  return key;
}

inline auto Value(std::size_t index, std::size_t bytes = 128) -> std::string {
  auto value = std::string(bytes, static_cast<char>('a' + index % 26U));
  const auto prefix = std::to_string(index) + ":";
  value.replace(0, std::min(prefix.size(), value.size()), prefix.substr(0, value.size()));
  return value;
}

inline auto Rows(Database &database,
                 KeyRange range = KeyRange::All()) -> Result<std::vector<std::pair<std::string, std::string>>> {
  auto read = database.BeginRead();
  if (!read) {
    return std::unexpected(read.error());
  }
  auto cursor = read->Scan(std::move(range));
  if (!cursor) {
    return std::unexpected(cursor.error());
  }
  auto rows = std::vector<std::pair<std::string, std::string>>{};
  while (cursor->Valid()) {
    auto value = cursor->CopyValue();
    if (!value) {
      return std::unexpected(value.error());
    }
    rows.emplace_back(cursor->Key(), std::move(*value));
    if (auto status = cursor->Next(); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }
  return rows;
}

template <typename Body>
void WithHook(io::TestHook hook, Body body) {
  [[maybe_unused]] const auto guard = ScopedTestHook(std::move(hook));
  body();
}

}  // namespace tinydb::test
