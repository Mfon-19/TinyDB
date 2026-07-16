#include "cache/committed_page_cache.h"

#include <tinydb/check.h>
#include <tinydb/disk_manager.h>

#include "btree/page_source.h"
#include "storage/page_codec.h"

#include <algorithm>
#include <atomic>
#include <expected>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace tinydb::cache {

struct CommittedFrame final {
  CommittedFrame(page_id_t initial_page_id, std::uint64_t initial_page_lsn, std::uint64_t initial_transaction_id,
                 std::unique_ptr<PageBytes> initial_bytes, bool initially_checkpointed)
      : page_id(initial_page_id),
        page_lsn(initial_page_lsn),
        transaction_id(initial_transaction_id),
        bytes(std::move(initial_bytes)),
        checkpointed(initially_checkpointed) {}

  page_id_t page_id;
  std::uint64_t page_lsn;
  std::uint64_t transaction_id;
  std::unique_ptr<PageBytes> bytes;

  // Pins and checkpoint status are cache metadata, not page contents. The
  // encoded bytes remain const after this object is constructed.
  mutable std::atomic<std::size_t> pin_count{0};
  std::atomic<bool> checkpointed{false};
};

PageGuard::PageGuard(std::shared_ptr<const CommittedFrame> frame) : frame_(std::move(frame)) {
  frame_->pin_count.fetch_add(1, std::memory_order_relaxed);
}

PageGuard::PageGuard(PageGuard &&other) noexcept : frame_(std::move(other.frame_)) {}

auto PageGuard::operator=(PageGuard &&other) noexcept -> PageGuard & {
  if (this != &other) {
    Reset();
    frame_ = std::move(other.frame_);
  }
  return *this;
}

PageGuard::~PageGuard() { Reset(); }

auto PageGuard::Id() const -> page_id_t {
  TINYDB_CHECK(frame_ != nullptr, "reading an empty committed-page guard");
  return frame_->page_id;
}

auto PageGuard::Data() const -> std::span<const char, PAGE_SIZE> {
  TINYDB_CHECK(frame_ != nullptr, "reading an empty committed-page guard");
  return *frame_->bytes;
}

auto PageGuard::PageLsn() const -> std::uint64_t {
  TINYDB_CHECK(frame_ != nullptr, "reading an empty committed-page guard");
  return frame_->page_lsn;
}

auto PageGuard::TransactionId() const -> std::uint64_t {
  TINYDB_CHECK(frame_ != nullptr, "reading an empty committed-page guard");
  return frame_->transaction_id;
}

void PageGuard::Reset() noexcept {
  if (frame_ == nullptr) {
    return;
  }
  const auto previous = frame_->pin_count.fetch_sub(1, std::memory_order_release);
  TINYDB_CHECK(previous != 0, "committed-page pin count underflow");
  frame_.reset();
}

auto PageGuard::IntoPageHandle() && -> PageHandle {
  TINYDB_CHECK(frame_ != nullptr, "transferring an empty committed-page guard");
  auto keeper = std::static_pointer_cast<const void>(frame_);
  auto *const frame = const_cast<CommittedFrame *>(frame_.get());
  const auto release = [](void *owner, page_id_t page_id, bool dirty) {
    auto *const released = static_cast<CommittedFrame *>(owner);
    TINYDB_CHECK(released->page_id == page_id, "committed-page lease changed identity");
    TINYDB_CHECK(!dirty, "immutable committed page was marked dirty");
    const auto previous = released->pin_count.fetch_sub(1, std::memory_order_release);
    TINYDB_CHECK(previous != 0, "committed-page pin count underflow");
  };
  frame_.reset();  // PageHandle now owns both the pin and shared lifetime.
  return PageHandle(frame, frame->page_id, frame->bytes->data(), release, std::move(keeper));
}

struct CommittedPageCache::Impl final {
  struct ResidentPage {
    std::shared_ptr<CommittedFrame> frame;
    std::list<page_id_t>::iterator lru_position;
  };

  DiskManager *disk;
  const std::size_t target_bytes;
  mutable std::mutex mutex;
  std::unordered_map<page_id_t, ResidentPage> pages;
  std::unordered_map<page_id_t, std::shared_ptr<CommittedFrame>> dirty_pages;
  std::list<page_id_t> lru;
  std::uint64_t checkpoint_lsn;

  Impl(DiskManager *database_file, std::size_t byte_target, std::uint64_t initial_checkpoint_lsn)
      : disk(database_file), target_bytes(byte_target), checkpoint_lsn(initial_checkpoint_lsn) {}

  void Touch(ResidentPage &page) {
    // Splice preserves the list node and iterator while moving this page to
    // the most-recently-used end of the policy.
    lru.splice(lru.begin(), lru, page.lru_position);
  }

  auto TryEvictOne() -> bool {
    for (auto candidate = lru.rbegin(); candidate != lru.rend(); ++candidate) {
      auto page = pages.find(*candidate);
      TINYDB_CHECK(page != pages.end(), "cache LRU references a missing page");
      const auto &frame = page->second.frame;
      if (frame->pin_count.load(std::memory_order_acquire) != 0 ||
          !frame->checkpointed.load(std::memory_order_acquire)) {
        continue;
      }

      const auto position = page->second.lru_position;
      dirty_pages.erase(page->first);
      pages.erase(page);
      lru.erase(position);
      return true;
    }
    return false;
  }

  void TrimToTarget() {
    while (pages.size() * PAGE_SIZE > target_bytes && TryEvictOne()) {
    }
  }

  void MakeRoomForRead() {
    while ((pages.size() + 1) * PAGE_SIZE > target_bytes && TryEvictOne()) {
    }
  }
};

CommittedPageCache::CommittedPageCache(DiskManager *disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn)
    : impl_(std::make_unique<Impl>(disk, target_bytes, checkpoint_lsn)) {
  TINYDB_CHECK(disk != nullptr, "committed cache requires a database file");
  TINYDB_CHECK(target_bytes >= PAGE_SIZE, "committed cache target must hold at least one page");
}

CommittedPageCache::~CommittedPageCache() = default;

auto CommittedPageCache::Read(page_id_t page_id) -> Result<PageGuard> {
  auto lock = std::lock_guard(impl_->mutex);
  if (auto existing = impl_->pages.find(page_id); existing != impl_->pages.end()) {
    impl_->Touch(existing->second);
    return PageGuard(existing->second.frame);
  }

  impl_->MakeRoomForRead();
  if ((impl_->pages.size() + 1) * PAGE_SIZE > impl_->target_bytes) {
    return std::unexpected(Status::ResourceExhausted("committed cache is full of pinned or uncheckpointed pages"));
  }

  auto bytes = std::make_unique<PageBytes>();
  if (auto status = impl_->disk->ReadPage(page_id, bytes->data()); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  const auto header = storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*bytes)), page_id);
  if (!header) {
    return std::unexpected(header.error());
  }

  auto frame = std::make_shared<CommittedFrame>(page_id, header->page_lsn, 0, std::move(bytes), true);
  impl_->lru.push_front(page_id);
  const auto [inserted, unique] =
      impl_->pages.emplace(page_id, Impl::ResidentPage{.frame = frame, .lru_position = impl_->lru.begin()});
  TINYDB_CHECK(unique, "cache miss raced with an existing page under its mutex");
  (void)inserted;
  return PageGuard(std::move(frame));
}

auto CommittedPageCache::Install(CommittedPageImage image) -> Status {
  if (image.bytes == nullptr) {
    return Status::InvalidArgument("committed page image has no bytes");
  }
  const auto header =
      storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*image.bytes)), image.page_id);
  if (!header) {
    return header.error();
  }
  if (header->page_lsn != image.page_lsn) {
    return Status::InvalidArgument("committed page metadata disagrees with encoded LSN");
  }

  auto lock = std::lock_guard(impl_->mutex);
  auto frame = std::make_shared<CommittedFrame>(image.page_id, image.page_lsn, image.transaction_id,
                                                std::move(image.bytes), image.page_lsn <= impl_->checkpoint_lsn);
  if (const auto existing = impl_->pages.find(image.page_id); existing != impl_->pages.end()) {
    if (existing->second.frame->page_lsn > image.page_lsn) {
      return Status::InvalidArgument("committed page version moves backward");
    }
    impl_->dirty_pages.erase(image.page_id);
    impl_->lru.erase(existing->second.lru_position);
    impl_->pages.erase(existing);
  }

  impl_->lru.push_front(image.page_id);
  impl_->pages.emplace(image.page_id, Impl::ResidentPage{.frame = frame, .lru_position = impl_->lru.begin()});
  if (!frame->checkpointed.load(std::memory_order_relaxed)) {
    impl_->dirty_pages.emplace(image.page_id, frame);
  }
  impl_->TrimToTarget();
  return {};
}

void CommittedPageCache::Retire(std::span<const page_id_t> page_ids) {
  auto lock = std::lock_guard(impl_->mutex);
  for (const auto page_id : page_ids) {
    const auto page = impl_->pages.find(page_id);
    if (page == impl_->pages.end()) {
      continue;
    }
    TINYDB_CHECK(page->second.frame->pin_count.load(std::memory_order_acquire) == 0,
                 "retiring a pinned committed page");
    impl_->dirty_pages.erase(page_id);
    impl_->lru.erase(page->second.lru_position);
    impl_->pages.erase(page);
  }
}

void CommittedPageCache::MarkCheckpointed(std::uint64_t checkpoint_lsn) {
  auto lock = std::lock_guard(impl_->mutex);
  TINYDB_CHECK(checkpoint_lsn >= impl_->checkpoint_lsn, "committed cache checkpoint frontier moved backward");
  impl_->checkpoint_lsn = checkpoint_lsn;

  for (auto page = impl_->dirty_pages.begin(); page != impl_->dirty_pages.end();) {
    if (page->second->page_lsn <= checkpoint_lsn) {
      page->second->checkpointed.store(true, std::memory_order_release);
      page = impl_->dirty_pages.erase(page);
    } else {
      ++page;
    }
  }
  impl_->TrimToTarget();
}

void CommittedPageCache::Trim() {
  auto lock = std::lock_guard(impl_->mutex);
  impl_->TrimToTarget();
}

auto CommittedPageCache::DirtyPageIds() const -> std::vector<page_id_t> {
  auto lock = std::lock_guard(impl_->mutex);
  auto result = std::vector<page_id_t>{};
  result.reserve(impl_->dirty_pages.size());
  for (const auto &[page_id, frame] : impl_->dirty_pages) {
    (void)frame;
    result.push_back(page_id);
  }
  std::ranges::sort(result);
  return result;
}

auto CommittedPageCache::DirtyPages() -> std::vector<PageGuard> {
  auto lock = std::lock_guard(impl_->mutex);
  auto ids = std::vector<page_id_t>{};
  ids.reserve(impl_->dirty_pages.size());
  for (const auto &[page_id, frame] : impl_->dirty_pages) {
    (void)frame;
    ids.push_back(page_id);
  }
  std::ranges::sort(ids);
  auto result = std::vector<PageGuard>{};
  result.reserve(ids.size());
  for (const auto page_id : ids) {
    result.push_back(PageGuard(impl_->dirty_pages.at(page_id)));
  }
  return result;
}

auto CommittedPageCache::Stats() const -> CommittedCacheStats {
  auto lock = std::lock_guard(impl_->mutex);
  auto pinned_pages = std::size_t{0};
  for (const auto &[page_id, page] : impl_->pages) {
    (void)page_id;
    pinned_pages += page.frame->pin_count.load(std::memory_order_acquire) != 0 ? 1U : 0U;
  }
  return CommittedCacheStats{
      .target_bytes = impl_->target_bytes,
      .resident_bytes = impl_->pages.size() * PAGE_SIZE,
      .resident_pages = impl_->pages.size(),
      .pinned_pages = pinned_pages,
      .dirty_pages = impl_->dirty_pages.size(),
      .checkpoint_lsn = impl_->checkpoint_lsn,
  };
}

}  // namespace tinydb::cache
