#include "cache/committed_page_cache.h"

#include "storage/disk_manager.h"
#include "util/check.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "storage/page_codec.h"

#include <algorithm>
#include <atomic>
#include <expected>
#include <limits>
#include <mutex>
#include <utility>

namespace tinydb::cache {

/*
** A frame owns exactly one immutable encoded page version. Its validated
** common header orders physical versions and travels with immutable handles,
** so consumers never checksum the same bytes twice. checkpointed controls
** eviction eligibility, not visibility.
*/
struct CommittedFrame final {
  using LastUnpin = void (*)(void *owner, CommittedFrame *frame) noexcept;

  CommittedFrame(storage::DataPageHeader initial_header, std::unique_ptr<PageBytes> initial_bytes,
                 bool initial_tree_payload_validated, bool initially_checkpointed, void *initial_eviction_owner,
                 LastUnpin initial_last_unpin)
      : page_id(initial_header.page_id),
        header(std::move(initial_header)),
        bytes(std::move(initial_bytes)),
        tree_payload_validated(initial_tree_payload_validated),
        checkpointed(initially_checkpointed),
        eviction_owner(initial_eviction_owner),
        last_unpin(initial_last_unpin) {}

  void ReleasePin() noexcept {
    const auto previous = pin_count.fetch_sub(1, std::memory_order_acq_rel);
    TINYDB_CHECK(previous != 0, "committed-page pin count underflow");
    // Dirty frames are not candidates. If checkpointing races this release,
    // either MarkCheckpointed observes zero pins and links the frame itself,
    // or this acquire observes its newly published checkpoint flag.
    if (previous == 1 && checkpointed.load(std::memory_order_acquire)) {
      last_unpin(eviction_owner, this);
    }
  }

  page_id_t page_id;                 // physical page identity
  storage::DataPageHeader header;    // checksum-authenticated common fields
  std::unique_ptr<PageBytes> bytes;  // immutable after construction
  bool tree_payload_validated;       // complete slot/cell proof for tree pages

  // Pins and checkpoint status are cache metadata, not page contents. The
  // encoded bytes remain const after this object is constructed.
  mutable std::atomic<std::size_t> pin_count{0};
  std::atomic<bool> checkpointed{false};

  /*
  ** EVICTABLE LRU LINKS
  **
  ** Only current, checkpointed, unpinned frames belong to this intrusive
  ** list.  newer points toward the MRU end and older toward the LRU end. The
  ** links live in the frame, so touching, pinning, unpinning, replacement,
  ** and victim removal never allocate.
  */
  CommittedFrame *newer{nullptr};
  CommittedFrame *older{nullptr};
  bool evictable{false};

  // PageHandle releases without knowing the cache's private Impl type.
  void *eviction_owner;
  LastUnpin last_unpin;
};

namespace {

auto Lease(std::shared_ptr<CommittedFrame> frame) -> PageHandle {
  TINYDB_CHECK(frame != nullptr, "leasing a null committed frame");
  auto keeper = std::static_pointer_cast<const void>(frame);
  auto *const leased = frame.get();
  const auto release = [](void *owner, page_id_t page_id, bool dirty) {
    auto *const released = static_cast<CommittedFrame *>(owner);
    TINYDB_CHECK(released->page_id == page_id, "committed-page lease changed identity");
    TINYDB_CHECK(!dirty, "immutable committed page was marked dirty");
    released->ReleasePin();
  };
  return PageHandle(leased, leased->page_id, leased->bytes->data(), release, std::move(keeper), &leased->header,
                    leased->tree_payload_validated);
}

}  // namespace

struct CommittedPageCache::Impl final {
  DiskManager *disk;  // backing checkpointed database file
  const std::size_t target_bytes;
  mutable std::mutex mutex;  // protects every field below
  std::vector<std::shared_ptr<CommittedFrame>> pages;
  std::size_t resident_pages{0};
  std::size_t dirty_pages{0};
  CommittedFrame *most_recent{nullptr};
  CommittedFrame *least_recent{nullptr};
  std::uint64_t checkpoint_lsn;  // eviction-safe durability frontier
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};

  Impl(DiskManager *database_file, std::size_t byte_target, std::uint64_t initial_checkpoint_lsn)
      : disk(database_file), target_bytes(byte_target), checkpoint_lsn(initial_checkpoint_lsn) {}

  static void LastUnpin(void *owner, CommittedFrame *frame) noexcept {
    static_cast<Impl *>(owner)->MakeEvictableAfterUnpin(frame);
  }

  void LinkMostRecent(CommittedFrame *frame) {
    TINYDB_CHECK(frame != nullptr && !frame->evictable && frame->newer == nullptr && frame->older == nullptr,
                 "linking a page already present in the eviction queue");
    TINYDB_CHECK(
        frame->pin_count.load(std::memory_order_acquire) == 0 && frame->checkpointed.load(std::memory_order_acquire),
        "linking a pinned or uncheckpointed eviction candidate");

    frame->older = most_recent;
    if (most_recent != nullptr) {
      most_recent->newer = frame;
    } else {
      least_recent = frame;
    }
    most_recent = frame;
    frame->evictable = true;
  }

  void Unlink(CommittedFrame *frame) {
    TINYDB_CHECK(frame != nullptr && frame->evictable, "unlinking a page absent from the eviction queue");
    if (frame->newer != nullptr) {
      frame->newer->older = frame->older;
    } else {
      TINYDB_CHECK(most_recent == frame, "eviction MRU link is inconsistent");
      most_recent = frame->older;
    }
    if (frame->older != nullptr) {
      frame->older->newer = frame->newer;
    } else {
      TINYDB_CHECK(least_recent == frame, "eviction LRU link is inconsistent");
      least_recent = frame->newer;
    }
    frame->newer = nullptr;
    frame->older = nullptr;
    frame->evictable = false;
  }

  void Pin(const std::shared_ptr<CommittedFrame> &frame) {
    // A first pin removes the frame from the victim queue. Further pins need
    // no queue work; the final release adds the current frame back at MRU.
    const auto previous = frame->pin_count.fetch_add(1, std::memory_order_acq_rel);
    if (previous == 0 && frame->evictable) {
      Unlink(frame.get());
    }
    TINYDB_CHECK(previous != std::numeric_limits<std::size_t>::max(), "committed-page pin count overflow");
  }

  void MakeEvictableAfterUnpin(CommittedFrame *frame) noexcept {
    auto lock = std::lock_guard(mutex);
    if (frame->pin_count.load(std::memory_order_acquire) != 0 || frame->evictable ||
        !frame->checkpointed.load(std::memory_order_acquire) || frame->page_id >= pages.size() ||
        pages[frame->page_id].get() != frame) {
      return;
    }
    LinkMostRecent(frame);
  }

  auto TryEvictOne() -> bool {
    // Queue membership is the eligibility test. The tail is therefore a
    // complete victim decision rather than the beginning of a page-table scan.
    auto *const victim = least_recent;
    if (victim == nullptr) {
      return false;
    }
    TINYDB_CHECK(victim->page_id < pages.size() && pages[victim->page_id].get() == victim,
                 "eviction queue contains a stale frame");
    Unlink(victim);
    pages[victim->page_id].reset();
    --resident_pages;
    ++evictions;
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

auto CommittedPageCache::Read(page_id_t page_id) -> Result<PageHandle> {
  auto lock = std::lock_guard(impl_->mutex);
  if (page_id < impl_->pages.size() && impl_->pages[page_id] != nullptr) {
    ++impl_->hits;
    impl_->Pin(impl_->pages[page_id]);
    return Lease(impl_->pages[page_id]);
  }
  ++impl_->misses;

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
    if (status.Code() == StatusCode::InvalidArgument) {
      return std::unexpected(Status::Corruption("tree references a page outside the allocated database"));
    }
    return std::unexpected(std::move(status));
  }
  const auto header = storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*bytes)), page_id);
  if (!header) {
    return std::unexpected(header.error());
  }
  const auto tree_page = header->type == storage::DataPageType::Leaf || header->type == storage::DataPageType::Internal;
  if (tree_page) {
    if (auto status = ValidateTreePagePayload(bytes->data(), *header); !status.Ok()) {
      return std::unexpected(std::move(status));
    }
  }

  auto frame =
      std::make_shared<CommittedFrame>(*header, std::move(bytes), tree_page, true, impl_.get(), &Impl::LastUnpin);
  if (page_id >= impl_->pages.size()) {
    impl_->pages.resize(page_id + 1);
  }
  TINYDB_CHECK(impl_->pages[page_id] == nullptr, "cache miss raced with an existing page under its mutex");
  impl_->pages[page_id] = frame;
  ++impl_->resident_pages;
  impl_->Pin(frame);
  return Lease(std::move(frame));
}

auto CommittedPageCache::PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                                            page_id_t high_water_page_id) -> Result<PublicationPlan> {
  if (high_water_page_id < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("publication has an invalid high-water page ID"));
  }

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
  auto previous_page_id = HEADER_PAGE_ID;
  for (auto &image : images) {
    if (image.bytes == nullptr || image.page_id < FIRST_DATA_PAGE_ID || image.page_id >= high_water_page_id ||
        image.page_id <= previous_page_id) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid or duplicate page image"));
    }
    previous_page_id = image.page_id;
    const auto header =
        storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*image.bytes)), image.page_id);
    if (!header) {
      return std::unexpected(header.error());
    }
    if (header->page_lsn != image.page_lsn) {
      return std::unexpected(Status::InvalidArgument("committed page metadata disagrees with encoded LSN"));
    }
    const auto tree_page =
        header->type == storage::DataPageType::Leaf || header->type == storage::DataPageType::Internal;
    if (tree_page) {
      if (auto status = ValidateTreePagePayload(image.bytes->data(), *header); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
    }
    const auto &existing = impl_->pages[image.page_id];
    if (existing != nullptr && existing->header.page_lsn > image.page_lsn) {
      return std::unexpected(Status::InvalidArgument("committed page version moves backward"));
    }
    frames.push_back(std::make_shared<CommittedFrame>(*header, std::move(image.bytes), tree_page,
                                                      image.page_lsn <= impl_->checkpoint_lsn, impl_.get(),
                                                      &Impl::LastUnpin));
  }
  previous_page_id = HEADER_PAGE_ID;
  auto image_index = std::size_t{0};
  for (const auto page_id : retired) {
    while (image_index < images.size() && images[image_index].page_id < page_id) {
      ++image_index;
    }
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= high_water_page_id || page_id <= previous_page_id ||
        (image_index < images.size() && images[image_index].page_id == page_id)) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid retired page"));
    }
    previous_page_id = page_id;
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
    if (impl_->pages[page_id]->evictable) {
      impl_->Unlink(impl_->pages[page_id].get());
    }
    impl_->dirty_pages -= !impl_->pages[page_id]->checkpointed.load(std::memory_order_acquire) ? 1U : 0U;
    impl_->pages[page_id].reset();
    --impl_->resident_pages;
  }
  for (auto &frame : plan.frames_) {
    auto &current = impl_->pages[frame->page_id];
    TINYDB_CHECK(current == nullptr || current->header.page_lsn <= frame->header.page_lsn,
                 "prepared publication regressed a page version");
    if (current == nullptr) {
      ++impl_->resident_pages;
    } else {
      impl_->dirty_pages -= !current->checkpointed.load(std::memory_order_acquire) ? 1U : 0U;
      if (current->evictable) {
        impl_->Unlink(current.get());
      }
    }
    current = std::move(frame);
    impl_->dirty_pages += !current->checkpointed.load(std::memory_order_acquire) ? 1U : 0U;
    if (current->checkpointed.load(std::memory_order_acquire) &&
        current->pin_count.load(std::memory_order_acquire) == 0) {
      impl_->LinkMostRecent(current.get());
    }
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
    if (page != nullptr && !page->checkpointed.load(std::memory_order_acquire) &&
        page->header.page_lsn <= checkpoint_lsn) {
      page->checkpointed.store(true, std::memory_order_release);
      TINYDB_CHECK(impl_->dirty_pages != 0, "committed cache dirty-page count underflow");
      --impl_->dirty_pages;
      if (page->pin_count.load(std::memory_order_acquire) == 0 && !page->evictable) {
        impl_->LinkMostRecent(page.get());
      }
    }
  }
  impl_->TrimToTarget();
}

auto CommittedPageCache::CaptureCheckpointPages(std::uint64_t checkpoint_lsn,
                                                std::uint64_t target_lsn) -> std::vector<PageHandle> {
  TINYDB_CHECK(checkpoint_lsn <= target_lsn, "checkpoint capture has a reversed LSN range");
  auto lock = std::lock_guard(impl_->mutex);
  // Return guards in page-ID order. A later publication may replace a table
  // entry, but the guard keeps this exact old immutable version alive until
  // checkpoint I/O finishes.
  auto result = std::vector<PageHandle>{};
  result.reserve(impl_->dirty_pages);
  for (const auto &page : impl_->pages) {
    if (page != nullptr && page->header.page_lsn > checkpoint_lsn && page->header.page_lsn <= target_lsn) {
      impl_->Pin(page);
      result.push_back(Lease(page));
    }
  }
  return result;
}

auto CommittedPageCache::DirtyPages() const -> std::size_t {
  auto lock = std::lock_guard(impl_->mutex);
  return impl_->dirty_pages;
}

auto CommittedPageCache::Stats() const -> CommittedCacheStats {
  auto lock = std::lock_guard(impl_->mutex);
  auto pinned_pages = std::size_t{0};
  for (const auto &page : impl_->pages) {
    if (page == nullptr) {
      continue;
    }
    pinned_pages += page->pin_count.load(std::memory_order_acquire) != 0 ? 1U : 0U;
  }
  return CommittedCacheStats{
      .target_bytes = impl_->target_bytes,
      .resident_bytes = impl_->resident_pages * PAGE_SIZE,
      .resident_pages = impl_->resident_pages,
      .pinned_pages = pinned_pages,
      .dirty_pages = impl_->dirty_pages,
      .hits = impl_->hits,
      .misses = impl_->misses,
      .evictions = impl_->evictions,
  };
}

}  // namespace tinydb::cache
