#include <tinydb/storage_engine.h>

#include <cstddef>
#include <cstdint>
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

// When the log outgrows this, the committing operation checkpoints: the
// pool and deferred metadata are flushed to the database file, fsynced,
// and the log starts over. Roughly 250 page images.
constexpr std::uint64_t CHECKPOINT_THRESHOLD_BYTES = 1U << 20U;
}  // namespace

auto StorageEngine::Open(const std::filesystem::path &path) -> Result<StorageEngine> {
  // Recovery runs before anything reads the database file: replay may
  // rewrite any page, including the header DiskManager::Open parses.
  const auto wal_path = Wal::PathFor(path);
  if (auto status = Wal::Recover(path, wal_path); !status.Ok()) {
    return std::unexpected(std::move(status));
  }

  auto disk_result = DiskManager::Open(path);
  if (!disk_result) {
    return std::unexpected(std::move(disk_result).error());
  }
  auto disk = std::make_unique<DiskManager>(*std::move(disk_result));
  auto pool = std::make_unique<BufferPool>(disk.get(), POOL_FRAME_COUNT);

  auto wal_result = Wal::Open(wal_path);
  if (!wal_result) {
    return std::unexpected(std::move(wal_result).error());
  }
  auto wal = std::make_unique<Wal>(*std::move(wal_result));

  // A fresh database has no root yet: allocate one zeroed page, record its
  // id in the in-memory header, and let the tree bootstrap it as an empty
  // leaf. None of this needs to be durable — the header change rides into
  // the first logged operation's images, and until then a crash just
  // re-bootstraps an equally empty database.
  auto root_page_id = disk->GetRootPageId();
  if (root_page_id == HEADER_PAGE_ID) {
    const auto new_page = pool->NewPage();
    if (!new_page) {
      return std::unexpected(new_page.error());
    }
    root_page_id = new_page->page_id;
    pool->UnpinPage(root_page_id, true);
    disk->SetRootPageId(root_page_id);
  }

  auto tree_result = BPlusTree::Open(pool.get(), root_page_id);
  if (!tree_result) {
    return std::unexpected(std::move(tree_result).error());
  }
  auto tree = std::make_unique<BPlusTree>(*std::move(tree_result));

  return StorageEngine(path, std::move(disk), std::move(pool), std::move(tree), std::move(wal));
}

StorageEngine::StorageEngine(std::filesystem::path path, std::unique_ptr<DiskManager> disk,
                             std::unique_ptr<BufferPool> pool, std::unique_ptr<BPlusTree> tree,
                             std::unique_ptr<Wal> wal)
    : path_(std::move(path)),
      disk_(std::move(disk)),
      pool_(std::move(pool)),
      tree_(std::move(tree)),
      wal_(std::move(wal)) {}

StorageEngine::StorageEngine(StorageEngine &&other) noexcept
    : path_(std::move(other.path_)),
      closed_(other.closed_),
      poisoned_(other.poisoned_),
      disk_(std::move(other.disk_)),
      pool_(std::move(other.pool_)),
      tree_(std::move(other.tree_)),
      wal_(std::move(other.wal_)) {
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
    wal_.reset();

    path_ = std::move(other.path_);
    closed_ = other.closed_;
    poisoned_ = other.poisoned_;
    disk_ = std::move(other.disk_);
    pool_ = std::move(other.pool_);
    tree_ = std::move(other.tree_);
    wal_ = std::move(other.wal_);
    other.closed_ = true;
  }

  return *this;
}

auto StorageEngine::Put(std::string_view key, std::string_view value) -> Status {
  if (closed_) {
    return Status::Closed("Put on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("Put on a poisoned handle; reopen the database to recover");
  }
  if (key.size() + value.size() > MAX_ENTRY_BYTES) {
    return Status::InvalidArgument("key + value exceeds MAX_ENTRY_BYTES");
  }

  pool_->BeginOp();
  if (auto status = tree_->Put(key, value); !status.Ok()) {
    Poison();
    return status;
  }
  return CommitOp();
}

auto StorageEngine::Get(std::string_view key) -> Result<std::optional<std::string>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Get on a closed handle"));
  }
  if (poisoned_) {
    return std::unexpected(Status::Closed("Get on a poisoned handle; reopen the database to recover"));
  }
  return tree_->Get(key);
}

auto StorageEngine::Remove(std::string_view key) -> Status {
  if (closed_) {
    return Status::Closed("Remove on a closed handle");
  }
  if (poisoned_) {
    return Status::Closed("Remove on a poisoned handle; reopen the database to recover");
  }

  pool_->BeginOp();
  if (auto status = tree_->Remove(key); !status.Ok()) {
    Poison();
    return status;
  }
  return CommitOp();
}

auto StorageEngine::Scan(std::string_view start,
                         std::string_view end) -> Result<std::vector<std::pair<std::string, std::string>>> {
  if (closed_) {
    return std::unexpected(Status::Closed("Scan on a closed handle"));
  }
  if (poisoned_) {
    return std::unexpected(Status::Closed("Scan on a poisoned handle; reopen the database to recover"));
  }
  return tree_->Scan(start, end);
}

auto StorageEngine::CommitOp() -> Status {
  const auto frames = pool_->OpDirtyFrames();
  const auto disk_images = disk_->TakeOpImages();

  // Nothing changed (say, removing an absent key): nothing to make
  // durable, and Wal::Commit treats an empty commit as a bug.
  if (frames.empty() && disk_images.empty()) {
    pool_->EndOp();
    return {};
  }

  for (const auto &[page_id, data] : frames) {
    wal_->AppendPageImage(page_id, data);
  }
  for (const auto &image : disk_images) {
    wal_->AppendPageImage(image.page_id, image.data.data());
  }
  if (auto status = wal_->Commit(); !status.Ok()) {
    Poison();
    return status;
  }
  // The images are durable: the frames go back to being ordinary dirty
  // pages, safe to evict.
  pool_->EndOp();

  if (wal_->SizeBytes() > CHECKPOINT_THRESHOLD_BYTES) {
    // The operation itself already committed; a checkpoint failure poisons
    // the handle but loses nothing — the log still holds every image.
    if (auto status = Checkpoint(); !status.Ok()) {
      Poison();
      return status;
    }
  }
  return {};
}

auto StorageEngine::Checkpoint() -> Status {
  if (auto status = pool_->FlushAllPages(); !status.Ok()) {
    return status;
  }
  if (auto status = disk_->Checkpoint(); !status.Ok()) {
    return status;
  }
  if (auto status = disk_->Sync(); !status.Ok()) {
    return status;
  }
  // Only now, with the database file caught up and durable, may the log
  // forget: truncating any earlier is data loss on the next crash.
  return wal_->Reset();
}

void StorageEngine::Poison() {
  poisoned_ = true;
  wal_->DiscardPending();
}

auto StorageEngine::Close() -> Status {
  if (!disk_) {  // fully closed already, or moved from
    return {};
  }
  closed_ = true;

  // A poisoned engine's in-memory state is not trustworthy, so none of it
  // is flushed as a checkpoint; the log already holds every committed
  // operation and the next Open() replays it. (The pool's destructor still
  // best-effort-writes committed-dirty frames, which is harmless: their
  // images stay in the log because the reset below never runs.)
  if (poisoned_) {
    tree_.reset();
    pool_.reset();
    disk_.reset();
    wal_.reset();
    return {};
  }

  // Release resources only after the checkpoint succeeds: if any step
  // fails, everything stays live and a second Close() retries, rewriting
  // only what has not already been written.
  if (auto status = Checkpoint(); !status.Ok()) {
    return status;
  }
  tree_.reset();
  pool_.reset();
  disk_.reset();
  wal_.reset();
  return {};
}
}  // namespace tinydb
