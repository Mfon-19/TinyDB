#include <tinydb/storage_engine.h>
#include <filesystem>
#include <string_view>

namespace tinydb {

StorageEngine::StorageEngine(std::filesystem::path path) : path_(std::move(path)) {}

StorageEngine::StorageEngine(StorageEngine &&other) noexcept
    : path_(std::move(other.path_)), closed_(other.closed_), data_(std::move(other.data_)) {
  // Mark the moved from StorageEngine as closed
  other.closed_ = true;
}

StorageEngine::~StorageEngine() { Close(); }

auto StorageEngine::operator=(StorageEngine &&other) noexcept -> StorageEngine & {
  if (this != &other) {
    // Close the current engine before moving resources
    Close();
    path_ = std::move(other.path_);
    closed_ = other.closed_;
    data_ = std::move(other.data_);
    other.closed_ = true;
  }

  return *this;
}

auto StorageEngine::Open(std::filesystem::path &path) -> StorageEngine {
  // Initialize a StorageEngine and return to the user
  return StorageEngine(path);
}

auto StorageEngine::Put(std::string_view key, std::string_view value) -> void {
  if (closed_) {
    return;
  }
  data_[std::string(key)] = std::string(value);
}

auto StorageEngine::Get(std::string_view key) const -> std::optional<std::string> {
  if (closed_) {
    return std::nullopt;
  }
  auto it = data_.find(std::string(key));
  if (it != data_.end()) {
    return it->second;
  }
  return std::nullopt;
}

auto StorageEngine::Remove(std::string_view key) -> void {
  if (closed_) {
    return;
  }
  data_.erase(std::string(key));
}

auto StorageEngine::Close() -> void {
  if (!closed_) {
    closed_ = true;
    data_.clear();  // Release backing store memory
  }
}
}  // namespace tinydb