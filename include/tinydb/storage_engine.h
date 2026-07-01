#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace tinydb {

// StorageEngine owns a TinyDB database handle and exposes the embedded
// key-value API.
//
// Handles are movable but not copyable. After Close(), writes are ignored and
// reads return std::nullopt.
class StorageEngine {
 public:
  StorageEngine(const StorageEngine &) = delete;
  auto operator=(const StorageEngine &) -> StorageEngine & = delete;

  StorageEngine(StorageEngine &&) noexcept;
  auto operator=(StorageEngine &&) noexcept -> StorageEngine &;
  ~StorageEngine();

  // Opens or creates the database at path.
  static auto Open(std::filesystem::path &path) -> StorageEngine;

  // Inserts key or replaces its existing value.
  auto Put(std::string_view key, std::string_view value) -> void;

  // Returns the value for key, or std::nullopt if key is missing or the handle
  // is closed.
  auto Get(std::string_view key) const -> std::optional<std::string>;

  // Removes key if it exists.
  auto Remove(std::string_view key) -> void;

  // Releases resources owned by this handle. Safe to call more than once.
  auto Close() -> void;

 private:
  // Construction goes through Open() so initialization and recovery can run
  // before callers receive a usable handle.
  explicit StorageEngine(std::filesystem::path);
  std::filesystem::path path_;
  bool closed_{false};

  // Temporary in-memory backing store until the disk-backed layers are wired
  // in.
  std::unordered_map<std::string, std::string> data_;
};
}  // namespace tinydb
