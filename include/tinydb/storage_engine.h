#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace tinydb {

class StorageEngine {
 public:
  // Delete the copy constructor and assignment
  StorageEngine(const StorageEngine &) = delete;
  auto operator=(const StorageEngine &) -> StorageEngine & = delete;

  // We allow the move constructor and assignment
  StorageEngine(StorageEngine &&) noexcept;
  auto operator=(StorageEngine &&) noexcept -> StorageEngine &;
  ~StorageEngine();

  static auto Open(std::filesystem::path &path) -> StorageEngine;
  auto Put(std::string_view key, std::string_view value) -> void;
  auto Get(std::string_view key) const -> std::optional<std::string>;
  auto Remove(std::string_view key) -> void;
  auto Close() -> void;

 private:
  // Control how users open a database using explicit
  // Something like: auto db = StorageEngine::Open("data.db");
  explicit StorageEngine(std::filesystem::path);
  std::filesystem::path path_;
  bool closed_{false};

  // temporary backing store
  std::unordered_map<std::string, std::string> data_;
};
}  // namespace tinydb