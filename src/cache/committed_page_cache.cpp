#include "cache/committed_page_cache.h"

#include "storage/disk_manager.h"
#include "util/check.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "storage/page_codec.h"

#include <expected>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace tinydb::cache {

/*
** A frame owns exactly one immutable encoded page version. Its validated
** common header orders physical versions and travels with immutable handles,
** so consumers never checksum the same bytes twice. checkpointed controls
** eviction eligibility, not visibility.
*/
struct CommittedFrame final {
  CommittedFrame(storage::DataPageHeader initial_header, std::unique_ptr<PageBytes> initial_bytes,
                 bool initial_tree_payload_validated, bool initially_checkpointed)
      : header(initial_header),
        bytes(std::move(initial_bytes)),
        tree_payload_validated(initial_tree_payload_validated),
        checkpointed(initially_checkpointed) {}

  storage::DataPageHeader header;    // checksum-authenticated common fields
  std::unique_ptr<PageBytes> bytes;  // immutable after construction
  bool tree_payload_validated;       // complete slot/cell proof for tree pages

  // Shared ownership supplies the pin count. The table owns one reference and
  // every PageHandle keepalive owns one more.
  bool checkpointed{false};

  /*
  ** CHECKPOINTED LRU LINKS
  **
  ** Every current checkpointed frame belongs to this intrusive list, including
  ** a temporarily pinned one. newer points toward the MRU end and older toward
  ** the LRU end. Eviction skips pins; release therefore only drops shared
  ** ownership and never reacquires the cache mutex.
  */
  CommittedFrame *newer{nullptr};
  CommittedFrame *older{nullptr};
  bool in_lru{false};
};

namespace {

auto Lease(std::shared_ptr<CommittedFrame> frame) -> PageHandle {
  TINYDB_CHECK(frame != nullptr, "leasing a null committed frame");
  auto *const leased = frame.get();
  auto keeper = std::static_pointer_cast<const void>(std::move(frame));
  return {leased->header.page_id, leased->bytes->data(), std::move(keeper), &leased->header,
          leased->tree_payload_validated};
}

}  // namespace

struct CommittedPageCache::Impl final {
  DiskManager *disk;  // backing checkpointed database file
  const std::size_t target_bytes;
  mutable std::mutex mutex;  // protects every field below
  std::vector<std::shared_ptr<CommittedFrame>> pages;
  std::size_t resident_pages{0};
  std::size_t loading_pages{0};
  std::size_t dirty_pages{0};
  CommittedFrame *most_recent{nullptr};
  CommittedFrame *least_recent{nullptr};
  std::uint64_t checkpoint_lsn;  // eviction-safe durability frontier
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};

  Impl(DiskManager *database_file, std::size_t byte_target, std::uint64_t initial_checkpoint_lsn)
      : disk(database_file), target_bytes(byte_target), checkpoint_lsn(initial_checkpoint_lsn) {}

  void LinkMostRecent(CommittedFrame *frame) {
    TINYDB_CHECK(frame != nullptr, "linking a null page into the LRU");
    TINYDB_CHECK(!frame->in_lru, "linking a page already present in the LRU");
    TINYDB_CHECK(frame->newer == nullptr, "linking a page with a stale newer LRU link");
    TINYDB_CHECK(frame->older == nullptr, "linking a page with a stale older LRU link");
    TINYDB_CHECK(frame->checkpointed, "linking an uncheckpointed eviction candidate");

    frame->older = most_recent;
    if (most_recent != nullptr) {
      most_recent->newer = frame;
    } else {
      least_recent = frame;
    }
    most_recent = frame;
    frame->in_lru = true;
  }

  void Unlink(CommittedFrame *frame) {
    TINYDB_CHECK(frame != nullptr, "unlinking a null page from the LRU");
    TINYDB_CHECK(frame->in_lru, "unlinking a page absent from the LRU");
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
    frame->in_lru = false;
  }

  void Touch(CommittedFrame *frame) {
    TINYDB_CHECK(frame != nullptr, "touching a null page in the LRU");
    TINYDB_CHECK(frame->in_lru, "touching a page absent from the LRU");
    if (frame == most_recent) {
      return;
    }
    Unlink(frame);
    LinkMostRecent(frame);
  }

  void RecordAccess(const std::shared_ptr<CommittedFrame> &frame) {
    if (frame->in_lru) {
      Touch(frame.get());
    }
  }

  auto TakeEvictionCandidate() -> std::shared_ptr<CommittedFrame> {
    // Pinned frames remain linked so their final release needs no mutex. In
    // ordinary use they are near the MRU end, leaving victim selection O(1);
    // a long-lived reader may require a short walk past its pinned frames.
    auto *victim = least_recent;
    while (victim != nullptr) {
      const auto page_id = victim->header.page_id;
      TINYDB_CHECK(page_id < pages.size(), "LRU page lies outside the cache table");
      TINYDB_CHECK(pages[page_id].get() == victim, "LRU contains a stale frame");
      if (pages[page_id].use_count() == 1) {
        break;
      }
      victim = victim->newer;
    }
    if (victim == nullptr) {
      return {};
    }
    const auto victim_page_id = victim->header.page_id;
    Unlink(victim);
    auto frame = std::move(pages[victim_page_id]);
    --resident_pages;
    ++evictions;
    return frame;
  }

  void TrimToTarget() {
    while (resident_pages + loading_pages > target_bytes / PAGE_SIZE) {
      if (TakeEvictionCandidate() == nullptr) {
        break;
      }
    }
  }

  auto MakeRoomForRead() -> std::shared_ptr<CommittedFrame> {
    auto reusable = std::shared_ptr<CommittedFrame>{};
    while (resident_pages + loading_pages >= target_bytes / PAGE_SIZE) {
      auto frame = TakeEvictionCandidate();
      if (frame == nullptr) {
        break;
      }
      if (reusable == nullptr) {
        reusable = std::move(frame);
      }
    }
    return reusable;
  }

  auto HasRoomForRead() const -> bool { return resident_pages + loading_pages < target_bytes / PAGE_SIZE; }

  /*
  ** Every blocking and page-sized operation in a physical miss happens here,
  ** outside mutex. An unobserved eviction victim supplies its existing aligned
  ** buffer, avoiding allocator churn in steady-state scans and random reads.
  */
  auto LoadFrame(page_id_t page_id,
                 std::shared_ptr<CommittedFrame> frame) const -> Result<std::shared_ptr<CommittedFrame>> {
    try {
      auto new_bytes = std::unique_ptr<PageBytes>{};
      if (frame == nullptr) {
        new_bytes = std::make_unique<PageBytes>();
      }
      auto *const bytes = frame != nullptr ? frame->bytes.get() : new_bytes.get();
      if (auto status = disk->ReadPage(page_id, bytes->data()); !status.Ok()) {
        if (status.Code() == StatusCode::InvalidArgument) {
          return std::unexpected(Status::Corruption("tree references a page outside the allocated database"));
        }
        return std::unexpected(std::move(status));
      }
      const auto header =
          storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*bytes)), page_id);
      if (!header) {
        return std::unexpected(header.error());
      }
      const auto tree_page =
          header->type == storage::DataPageType::Leaf || header->type == storage::DataPageType::Internal;
      if (tree_page) {
        if (auto status = ValidateTreePagePayload(bytes->data(), *header); !status.Ok()) {
          return std::unexpected(std::move(status));
        }
      }

      if (frame != nullptr) {
        frame->header = *header;
        frame->tree_payload_validated = tree_page;
        frame->checkpointed = true;
      } else {
        frame = std::make_shared<CommittedFrame>(*header, std::move(new_bytes), tree_page, true);
      }
      return frame;
    } catch (const std::bad_alloc &) {
      return std::unexpected(Status::ResourceExhausted("committed page load allocation failed"));
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
  auto reusable = std::shared_ptr<CommittedFrame>{};
  {
    auto lock = std::unique_lock(impl_->mutex);
    if (page_id < impl_->pages.size() && impl_->pages[page_id] != nullptr) {
      ++impl_->hits;
      impl_->RecordAccess(impl_->pages[page_id]);
      return Lease(impl_->pages[page_id]);
    }
    ++impl_->misses;

    // A loading page consumes the same hard capacity as a resident page.
    // Publication alone may temporarily exceed the target with dirty pages.
    reusable = impl_->MakeRoomForRead();
    if (!impl_->HasRoomForRead()) {
      return std::unexpected(
          Status::ResourceExhausted("committed cache is full of pinned, loading, or uncheckpointed pages"));
    }
    ++impl_->loading_pages;
  }

  auto loaded = impl_->LoadFrame(page_id, std::move(reusable));
  auto lock = std::unique_lock(impl_->mutex);
  TINYDB_CHECK(impl_->loading_pages != 0, "committed-page loading count underflow");
  --impl_->loading_pages;

  // A racing miss for the same page may have installed first. Reuse that
  // immutable version and discard this duplicate read instead of replacing it.
  if (page_id < impl_->pages.size() && impl_->pages[page_id] != nullptr) {
    impl_->RecordAccess(impl_->pages[page_id]);
    return Lease(impl_->pages[page_id]);
  }
  if (!loaded) {
    lock.unlock();
    return std::unexpected(std::move(loaded.error()));
  }
  if (page_id >= impl_->pages.size()) {
    try {
      impl_->pages.resize(page_id + 1);
    } catch (const std::bad_alloc &) {
      return std::unexpected(Status::ResourceExhausted("committed cache page table allocation failed"));
    } catch (const std::length_error &) {
      return std::unexpected(Status::ResourceExhausted("committed cache page table exceeds container limits"));
    }
  }
  auto &current = impl_->pages[page_id];
  current = std::move(*loaded);
  ++impl_->resident_pages;
  impl_->LinkMostRecent(current.get());
  return Lease(current);
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
                                                      image.page_lsn <= impl_->checkpoint_lsn));
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
    TINYDB_CHECK(impl_->pages[page_id].use_count() == 1, "retiring a pinned committed page");
    if (impl_->pages[page_id]->in_lru) {
      impl_->Unlink(impl_->pages[page_id].get());
    }
    impl_->dirty_pages -= !impl_->pages[page_id]->checkpointed ? 1U : 0U;
    impl_->pages[page_id].reset();
    --impl_->resident_pages;
  }
  for (auto &frame : plan.frames_) {
    auto &current = impl_->pages[frame->header.page_id];
    if (current != nullptr) {
      TINYDB_CHECK(current->header.page_lsn <= frame->header.page_lsn, "prepared publication regressed a page version");
    }
    if (current == nullptr) {
      ++impl_->resident_pages;
    } else {
      impl_->dirty_pages -= !current->checkpointed ? 1U : 0U;
      if (current->in_lru) {
        impl_->Unlink(current.get());
      }
    }
    current = std::move(frame);
    impl_->dirty_pages += !current->checkpointed ? 1U : 0U;
    if (current->checkpointed) {
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
    if (page != nullptr && !page->checkpointed && page->header.page_lsn <= checkpoint_lsn) {
      page->checkpointed = true;
      TINYDB_CHECK(impl_->dirty_pages != 0, "committed cache dirty-page count underflow");
      --impl_->dirty_pages;
      if (!page->in_lru) {
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
      impl_->RecordAccess(page);
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
    pinned_pages += page.use_count() > 1 ? 1U : 0U;
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
