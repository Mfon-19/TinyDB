#include <tinydb/storage_engine.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb {

namespace {
constexpr std::size_t POOL_FRAME_COUNT = 64;
}  // namespace

StorageEngine::StorageEngine(std::filesystem::path path)
    : path_(std::move(path)),
      disk_(std::make_unique<DiskManager>(path_)),
      pool_(std::make_unique<BufferPool>(disk_.get(), POOL_FRAME_COUNT)) {
  // A fresh database has no root yet: allocate one zeroed page, persist its
  // id in the file header, and let the tree bootstrap it as an empty leaf.
  auto root_page_id = disk_->GetRootPageId();
  if (root_page_id == HEADER_PAGE_ID) {
    pool_->NewPage(&root_page_id);
    pool_->UnpinPage(root_page_id, true);
    disk_->SetRootPageId(root_page_id);
  }
  tree_ = std::make_unique<BPlusTree>(pool_.get(), root_page_id);
}

StorageEngine::StorageEngine(StorageEngine &&other) noexcept
    : path_(std::move(other.path_)),
      closed_(other.closed_),
      disk_(std::move(other.disk_)),
      pool_(std::move(other.pool_)),
      tree_(std::move(other.tree_)) {
  // Mark the moved from StorageEngine as closed
  other.closed_ = true;
}

StorageEngine::~StorageEngine() { CloseBestEffort(); }

void StorageEngine::CloseBestEffort() noexcept {
  try {
    Close();
  } catch (const std::exception &error) {
    // A destructor and a noexcept move must not throw. Callers who need to
    // handle flush errors (disk full, I/O failure) should call Close()
    // themselves.
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", path_.c_str(), error.what());
  }
}

auto StorageEngine::operator=(StorageEngine &&other) noexcept -> StorageEngine & {
  if (this != &other) {
    CloseBestEffort();
    // If the close failed, the members are still live: release them in
    // dependency order (the pool's destructor retries its flush while the
    // disk manager is still alive) rather than letting the member-wise
    // assignments below destroy the disk manager out from under the pool.
    tree_.reset();
    pool_.reset();
    disk_.reset();

    path_ = std::move(other.path_);
    closed_ = other.closed_;
    disk_ = std::move(other.disk_);
    pool_ = std::move(other.pool_);
    tree_ = std::move(other.tree_);
    other.closed_ = true;
  }

  return *this;
}

auto StorageEngine::Open(const std::filesystem::path &path) -> StorageEngine { return StorageEngine(path); }

auto StorageEngine::Put(std::string_view key, std::string_view value) -> PutStatus {
  if (closed_) {
    return PutStatus::Closed;
  }
  if (key.size() + value.size() > MAX_ENTRY_BYTES) {
    return PutStatus::EntryTooLarge;
  }
  tree_->Put(key, value);
  return PutStatus::Ok;
}

auto StorageEngine::Get(std::string_view key) -> std::optional<std::string> {
  if (closed_) {
    return std::nullopt;
  }
  return tree_->Get(key);
}

auto StorageEngine::Remove(std::string_view key) -> void {
  if (closed_) {
    return;
  }
  tree_->Remove(key);
}

auto StorageEngine::Scan(std::string_view start,
                         std::string_view end) -> std::vector<std::pair<std::string, std::string>> {
  if (closed_) {
    return {};
  }
  return tree_->Scan(start, end);
}

auto StorageEngine::Close() -> void {
  if (!disk_) {  // fully closed already, or moved from
    return;
  }
  // Reject reads and writes from here on, but release resources only after
  // the flush and sync succeed: if either throws, everything stays live and
  // a second Close() retries (FlushAllPages marks frames clean as they are
  // written, so only the remainder is rewritten).
  closed_ = true;
  pool_->FlushAllPages();
  disk_->Sync();  // the durability point: writes reach the device, not just the page cache
  tree_.reset();
  pool_.reset();
  disk_.reset();
}
}  // namespace tinydb
