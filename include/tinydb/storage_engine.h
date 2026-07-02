#pragma once

#include <tinydb/b_plus_tree.h>
#include <tinydb/buffer_pool.h>
#include <tinydb/disk_manager.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

// Result of a write. Rejected writes leave the database unchanged.
enum class PutStatus {
  Ok,
  EntryTooLarge,  // key + value exceeds MAX_ENTRY_BYTES
  Closed,         // the handle was closed
};

// StorageEngine owns a TinyDB database handle and exposes the embedded
// key-value API. It wires the full stack: a DiskManager for page I/O, a
// BufferPool caching pages above it, and a B+ tree indexing the data.
//
// Handles are movable but not copyable. After Close(), writes report
// PutStatus::Closed and reads return std::nullopt.
class StorageEngine {
 public:
  StorageEngine(const StorageEngine &) = delete;
  auto operator=(const StorageEngine &) -> StorageEngine & = delete;

  StorageEngine(StorageEngine &&) noexcept;
  auto operator=(StorageEngine &&) noexcept -> StorageEngine &;
  ~StorageEngine();

  // Opens or creates the database at path. Throws on I/O failure or if the
  // file is not a TinyDB database.
  static auto Open(const std::filesystem::path &path) -> StorageEngine;

  // Inserts key or replaces its existing value. This is the layer that
  // enforces the entry size cap; the tree below assumes it holds.
  auto Put(std::string_view key, std::string_view value) -> PutStatus;

  // Returns the value for key, or std::nullopt if key is missing or the
  // handle is closed. Not const: reads pin pages in the buffer pool.
  auto Get(std::string_view key) -> std::optional<std::string>;

  // Removes key if it exists.
  auto Remove(std::string_view key) -> void;

  // Returns rows with start <= key < end, in key order.
  auto Scan(std::string_view start, std::string_view end)
      -> std::vector<std::pair<std::string, std::string>>;

  // Flushes everything to disk and releases resources owned by this handle.
  // Safe to call more than once.
  auto Close() -> void;

 private:
  // Construction goes through Open() so initialization and recovery can run
  // before callers receive a usable handle.
  explicit StorageEngine(std::filesystem::path path);

  std::filesystem::path path_;
  bool closed_{false};

  // The pool holds a pointer to the disk manager and the tree a pointer to
  // the pool, so member order fixes construction/destruction order and
  // unique_ptr keeps the addresses stable across moves.
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<BPlusTree> tree_;
};
}  // namespace tinydb
