#include "cache/committed_page_cache.h"

#include <tinydb/check.h>
#include <tinydb/disk_manager.h>

#include "btree/page_source.h"
#include "storage/page_codec.h"

#include <algorithm>
#include <atomic>
#include <expected>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace tinydb::cache {

/*
** A frame owns exactly one immutable encoded page version. page_lsn orders
** physical versions of the same page; transaction_id associates the version
** with its logical publication. checkpointed controls eviction eligibility,
** not visibility.
*/
struct CommittedFrame final {
  CommittedFrame(page_id_t initial_page_id, std::uint64_t initial_page_lsn, std::uint64_t initial_transaction_id,
                 std::unique_ptr<PageBytes> initial_bytes, bool initially_checkpointed)
      : page_id(initial_page_id),
        page_lsn(initial_page_lsn),
        transaction_id(initial_transaction_id),
        bytes(std::move(initial_bytes)),
        checkpointed(initially_checkpointed) {}

  page_id_t page_id;                 // physical page identity
  std::uint64_t page_lsn;            // physical version ordering
  std::uint64_t transaction_id;      // logical publication that installed it
  std::unique_ptr<PageBytes> bytes;  // immutable after construction

  // Pins and checkpoint status are cache metadata, not page contents. The
  // encoded bytes remain const after this object is constructed.
  mutable std::atomic<std::size_t> pin_count{0};
  std::atomic<bool> checkpointed{false};
  std::uint64_t last_access{0};  // protected by the cache mutex
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
  // PageHandle type-erases the shared owner. keeper preserves the frame after
  // this guard relinquishes its shared_ptr, while release transfers the exact
  // existing pin instead of incrementing and decrementing around conversion.
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
  DiskManager *disk;  // backing checkpointed database file
  const std::size_t target_bytes;
  mutable std::mutex mutex;  // protects every field below
  std::vector<std::shared_ptr<CommittedFrame>> pages;
  std::size_t resident_pages{0};
  std::uint64_t access_clock{0};
  std::uint64_t checkpoint_lsn;  // eviction-safe durability frontier

  Impl(DiskManager *database_file, std::size_t byte_target, std::uint64_t initial_checkpoint_lsn)
      : disk(database_file), target_bytes(byte_target), checkpoint_lsn(initial_checkpoint_lsn) {}

  void Touch(const std::shared_ptr<CommittedFrame> &page) {
    ++access_clock;
    page->last_access = access_clock;
  }

  auto TryEvictOne() -> bool {
    // A dense page table removes node allocation from publication. Eviction
    // scans for the oldest eligible frame; cache capacity is intentionally a
    // memory budget, not an asymptotic complexity promise.
    auto victim = pages.size();
    auto oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < pages.size(); ++index) {
      const auto &frame = pages[index];
      if (frame == nullptr) {
        continue;
      }
      if (frame->pin_count.load(std::memory_order_acquire) != 0 ||
          !frame->checkpointed.load(std::memory_order_acquire)) {
        continue;
      }
      if (frame->last_access < oldest) {
        oldest = frame->last_access;
        victim = index;
      }
    }
    if (victim == pages.size()) {
      return false;
    }
    pages[victim].reset();
    --resident_pages;
    return true;
  }

  void TrimToTarget() {
    while (resident_pages * PAGE_SIZE > target_bytes && TryEvictOne()) {
    }
  }

  void MakeRoomForRead() {
    while ((resident_pages + 1) * PAGE_SIZE > target_bytes && TryEvictOne()) {
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
  if (page_id < impl_->pages.size() && impl_->pages[page_id] != nullptr) {
    impl_->Touch(impl_->pages[page_id]);
    return PageGuard(impl_->pages[page_id]);
  }

  // Disk reads obey the hard target. Unlike publication, a cache miss has no
  // already-approved transaction overage and may fail if every frame is pinned
  // or waiting for checkpoint.
  impl_->MakeRoomForRead();
  if ((impl_->resident_pages + 1) * PAGE_SIZE > impl_->target_bytes) {
    return std::unexpected(Status::ResourceExhausted("committed cache is full of pinned or uncheckpointed pages"));
  }

  // The cache mutex remains held across I/O. This deliberately serializes a
  // miss so a second reader cannot load and install a duplicate frame.
  auto bytes = std::make_unique<PageBytes>();
  if (auto status = impl_->disk->ReadPage(page_id, bytes->data()); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  const auto header = storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*bytes)), page_id);
  if (!header) {
    return std::unexpected(header.error());
  }

  auto frame = std::make_shared<CommittedFrame>(page_id, header->page_lsn, 0, std::move(bytes), true);
  if (page_id >= impl_->pages.size()) {
    impl_->pages.resize(page_id + 1);
  }
  TINYDB_CHECK(impl_->pages[page_id] == nullptr, "cache miss raced with an existing page under its mutex");
  impl_->pages[page_id] = frame;
  ++impl_->resident_pages;
  impl_->Touch(frame);
  return PageGuard(std::move(frame));
}

auto CommittedPageCache::PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                                            page_id_t high_water_page_id) -> Result<PublicationPlan> {
  if (high_water_page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("publication has an invalid high-water page ID"));
  }

  auto seen = std::unordered_set<page_id_t>{};
  seen.reserve(images.size());
  auto frames = std::vector<std::shared_ptr<CommittedFrame>>{};
  frames.reserve(images.size());

  /*
  ** PREPARE CACHE OWNERSHIP
  **
  ** Grow the dense table and allocate every shared control block here. The
  ** writer has not appended WAL yet, so any validation or allocation failure
  ** remains a definite abort.
  */
  auto lock = std::lock_guard(impl_->mutex);
  if (impl_->pages.size() < high_water_page_id) {
    impl_->pages.resize(high_water_page_id);
  }
  for (auto &image : images) {
    if (image.bytes == nullptr || image.page_id < FIRST_DATA_PAGE_ID || image.page_id >= high_water_page_id ||
        !seen.insert(image.page_id).second) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid or duplicate page image"));
    }
    const auto header =
        storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*image.bytes)), image.page_id);
    if (!header) {
      return std::unexpected(header.error());
    }
    if (header->page_lsn != image.page_lsn) {
      return std::unexpected(Status::InvalidArgument("committed page metadata disagrees with encoded LSN"));
    }
    const auto &existing = impl_->pages[image.page_id];
    if (existing != nullptr && existing->page_lsn > image.page_lsn) {
      return std::unexpected(Status::InvalidArgument("committed page version moves backward"));
    }
    frames.push_back(std::make_shared<CommittedFrame>(image.page_id, image.page_lsn, image.transaction_id,
                                                      std::move(image.bytes), image.page_lsn <= impl_->checkpoint_lsn));
  }
  for (const auto page_id : retired) {
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= high_water_page_id || seen.contains(page_id)) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid retired page"));
    }
  }
  return PublicationPlan(std::move(frames), std::move(retired), high_water_page_id);
}

void CommittedPageCache::Publish(PublicationPlan plan) noexcept {
  auto lock = std::lock_guard(impl_->mutex);
  TINYDB_CHECK(plan.high_water_page_id_ <= impl_->pages.size(), "publication page table was not preallocated");

  for (const auto page_id : plan.retired_) {
    if (impl_->pages[page_id] == nullptr) {
      continue;
    }
    TINYDB_CHECK(impl_->pages[page_id]->pin_count.load(std::memory_order_acquire) == 0,
                 "retiring a pinned committed page");
    impl_->pages[page_id].reset();
    --impl_->resident_pages;
  }
  for (auto &frame : plan.frames_) {
    auto &current = impl_->pages[frame->page_id];
    TINYDB_CHECK(current == nullptr || current->page_lsn <= frame->page_lsn,
                 "prepared publication regressed a page version");
    if (current == nullptr) {
      ++impl_->resident_pages;
    }
    current = std::move(frame);
    impl_->Touch(current);
  }
  impl_->TrimToTarget();
}

void CommittedPageCache::MarkCheckpointed(std::uint64_t checkpoint_lsn) {
  auto lock = std::lock_guard(impl_->mutex);
  TINYDB_CHECK(checkpoint_lsn >= impl_->checkpoint_lsn, "committed cache checkpoint frontier moved backward");
  impl_->checkpoint_lsn = checkpoint_lsn;

  // Only versions at or below the new frontier are now reproducible from the
  // database file. Later committed versions remain WAL-backed and non-evictable.
  for (const auto &page : impl_->pages) {
    if (page != nullptr && page->page_lsn <= checkpoint_lsn) {
      page->checkpointed.store(true, std::memory_order_release);
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
  for (page_id_t page_id = 0; page_id < impl_->pages.size(); ++page_id) {
    const auto &page = impl_->pages[page_id];
    if (page != nullptr && !page->checkpointed.load(std::memory_order_acquire)) {
      result.push_back(page_id);
    }
  }
  return result;
}

auto CommittedPageCache::DirtyPages() -> std::vector<PageGuard> {
  auto lock = std::lock_guard(impl_->mutex);
  // Return guards in page-ID order. Checkpointing receives a stable version of
  // every frame even if a later publication replaces the page-table entry.
  auto result = std::vector<PageGuard>{};
  for (const auto &page : impl_->pages) {
    if (page != nullptr && !page->checkpointed.load(std::memory_order_acquire)) {
      result.push_back(PageGuard(page));
    }
  }
  return result;
}

auto CommittedPageCache::Stats() const -> CommittedCacheStats {
  auto lock = std::lock_guard(impl_->mutex);
  auto pinned_pages = std::size_t{0};
  auto dirty_pages = std::size_t{0};
  for (const auto &page : impl_->pages) {
    if (page == nullptr) {
      continue;
    }
    pinned_pages += page->pin_count.load(std::memory_order_acquire) != 0 ? 1U : 0U;
    dirty_pages += !page->checkpointed.load(std::memory_order_acquire) ? 1U : 0U;
  }
  return CommittedCacheStats{
      .target_bytes = impl_->target_bytes,
      .resident_bytes = impl_->resident_pages * PAGE_SIZE,
      .resident_pages = impl_->resident_pages,
      .pinned_pages = pinned_pages,
      .dirty_pages = dirty_pages,
      .checkpoint_lsn = impl_->checkpoint_lsn,
  };
}

}  // namespace tinydb::cache
