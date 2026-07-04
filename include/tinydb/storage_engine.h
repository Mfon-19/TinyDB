#pragma once

#include <tinydb/b_plus_tree.h>
#include <tinydb/buffer_pool.h>
#include <tinydb/disk_manager.h>
#include <tinydb/status.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

// StorageEngine owns a TinyDB database handle and exposes the embedded
// key-value API. It wires the full stack: a DiskManager for page I/O, a
// BufferPool caching pages above it, and a B+ tree indexing the data.
//
// Failures travel as Status / Result values (see status.h); nothing throws.
// Handles are movable but not copyable. After Close(), every operation
// fails with StatusCode::Closed.
class StorageEngine {
 public:
  StorageEngine(const StorageEngine &) = delete;
  auto operator=(const StorageEngine &) -> StorageEngine & = delete;

  StorageEngine(StorageEngine &&other) noexcept;
  auto operator=(StorageEngine &&other) noexcept -> StorageEngine &;
  ~StorageEngine();

  // Opens or creates the database at path. Fails with InvalidArgument if
  // the file is not a TinyDB database and IoError on environment failures.
  static auto Open(const std::filesystem::path &path) -> Result<StorageEngine>;

  // Inserts key or replaces its existing value. Fails with InvalidArgument
  // if key + value exceeds MAX_ENTRY_BYTES; this is the layer that enforces
  // the cap, the tree below assumes it holds.
  auto Put(std::string_view key, std::string_view value) -> Status;

  // Returns the value stored under key, or std::nullopt if key is missing.
  // Not const: reads pin pages in the buffer pool.
  auto Get(std::string_view key) -> Result<std::optional<std::string>>;

  // Removes key if it exists.
  auto Remove(std::string_view key) -> Status;

  // Returns rows with start <= key < end, in key order.
  auto Scan(std::string_view start, std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>>;

  // Flushes everything to the storage device (fsync) and releases resources
  // owned by this handle. Safe to call more than once. On failure the
  // handle rejects reads and writes but keeps its resources, so Close() can
  // be called again to retry the flush. The destructor calls this too but
  // can only report errors to stderr, so call Close() directly when flush
  // errors must be handled.
  auto Close() -> Status;

 private:
  StorageEngine(std::filesystem::path path, std::unique_ptr<DiskManager> disk, std::unique_ptr<BufferPool> pool,
                std::unique_ptr<BPlusTree> tree);

  // Close() for the destructor and move assignment, which cannot surface a
  // status: failures are reported to stderr and dropped.
  void CloseBestEffort() noexcept;

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
