#include <tinydb/storage_engine.h>

#include <cstdio>
#include <expected>
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

auto StorageEngine::Open(const std::filesystem::path &path) -> Result<StorageEngine> {
  auto disk_result = DiskManager::Open(path);
  if (!disk_result) {
    return std::unexpected(std::move(disk_result).error());
  }
  auto disk = std::make_unique<DiskManager>(*std::move(disk_result));
  auto pool = std::make_unique<BufferPool>(disk.get(), POOL_FRAME_COUNT);

  // A fresh database has no root yet: allocate one zeroed page, persist its
  // id in the file header, and let the tree bootstrap it as an empty leaf.
  auto root_page_id = disk->GetRootPageId();
  if (root_page_id == HEADER_PAGE_ID) {
    const auto new_page = pool->NewPage();
    if (!new_page) {
      return std::unexpected(new_page.error());
    }
    root_page_id = new_page->page_id;
    pool->UnpinPage(root_page_id, true);
    if (auto status = disk->SetRootPageId(root_page_id); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }

  auto tree_result = BPlusTree::Open(pool.get(), root_page_id);
  if (!tree_result) {
    return std::unexpected(std::move(tree_result).error());
  }
  auto tree = std::make_unique<BPlusTree>(*std::move(tree_result));

  return StorageEngine(path, std::move(disk), std::move(pool), std::move(tree));
}

StorageEngine::StorageEngine(std::filesystem::path path, std::unique_ptr<DiskManager> disk,
                             std::unique_ptr<BufferPool> pool, std::unique_ptr<BPlusTree> tree)
    : path_(std::move(path)), disk_(std::move(disk)), pool_(std::move(pool)), tree_(std::move(tree)) {}

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
  if (const auto status = Close(); !status.Ok()) {
    std::fprintf(stderr, "tinydb: failed to close %s: %s\n", path_.c_str(), status.ToString().c_str());
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

auto StorageEngine::Put(std::string_view key, std::string_view value) -> Status {
  if (closed_) {
    return Status::Closed("Put on a closed handle");
  }
  if (key.size() + value.size() > MAX_ENTRY_BYTES) {
    return Status::InvalidArgument("key + value exceeds MAX_ENTRY_BYTES");
  }
  return tree_->Put(key, value);
}

auto StorageEngine::Get(std::string_view key) -> Result<std::optional<std::string>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Get on a closed handle"));
  }
  return tree_->Get(key);
}

auto StorageEngine::Remove(std::string_view key) -> Status {
  if (closed_) {
    return Status::Closed("Remove on a closed handle");
  }
  return tree_->Remove(key);
}

auto StorageEngine::Scan(std::string_view start,
                         std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Scan on a closed handle"));
  }
  return tree_->Scan(start, end);
}

auto StorageEngine::Close() -> Status {
  if (!disk_) {  // fully closed already, or moved from
    return {};
  }
  // Reject reads and writes from here on, but release resources only after
  // the flush and sync succeed: if either fails, everything stays live and
  // a second Close() retries (FlushAllPages marks frames clean as they are
  // written, so only the remainder is rewritten).
  closed_ = true;
  if (auto status = pool_->FlushAllPages(); !status.Ok()) {
    return status;
  }
  // The durability point: writes reach the device, not just the page cache.
  if (auto status = disk_->Sync(); !status.Ok()) {
    return status;
  }
  tree_.reset();
  pool_.reset();
  disk_.reset();
  return {};
}
}  // namespace tinydb
