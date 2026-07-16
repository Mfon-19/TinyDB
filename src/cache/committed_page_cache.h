#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace tinydb {

class DiskManager;
class PageHandle;

namespace cache {

using PageBytes = std::array<char, PAGE_SIZE>;

struct CommittedFrame;

/*
** IMMUTABLE COMMITTED PAGE CACHE
**
** The page table contains only the newest visible committed version of each
** page. Installed frame bytes never change. Replacing a page swaps the table's
** shared frame pointer, while an older PageGuard can continue reading the old
** immutable frame until its snapshot drains.
**
** Frames newer than checkpoint_lsn are dirty in the cache sense: WAL contains
** their durable image, but the database file does not yet. Such frames cannot
** be evicted. The byte target is consequently soft; publication may exceed it
** until checkpointing advances the frontier.
**
** PageGuard is the only cache-facing byte lease. It is move-only so one cache
** pin has one owner and one release point.
*/
class PageGuard final {
 public:
  PageGuard() = default;
  PageGuard(const PageGuard &) = delete;
  auto operator=(const PageGuard &) -> PageGuard & = delete;
  PageGuard(PageGuard &&other) noexcept;
  auto operator=(PageGuard &&other) noexcept -> PageGuard &;
  ~PageGuard();

  auto Id() const -> page_id_t;
  auto Data() const -> std::span<const char, PAGE_SIZE>;
  auto PageLsn() const -> std::uint64_t;
  auto TransactionId() const -> std::uint64_t;

  // Transfers this exact pin into the tree's generic page lease. The frame's
  // shared owner keeps old immutable bytes alive after cache replacement.
  auto IntoPageHandle() && -> PageHandle;
  explicit operator bool() const noexcept { return frame_ != nullptr; }

 private:
  explicit PageGuard(std::shared_ptr<const CommittedFrame> frame);
  void Reset() noexcept;

  std::shared_ptr<const CommittedFrame> frame_;

  friend class CommittedPageCache;
};

/* A frozen transaction transfers this allocation directly into a frame. */
struct CommittedPageImage {
  page_id_t page_id{HEADER_PAGE_ID};
  std::uint64_t page_lsn{0};
  std::uint64_t transaction_id{0};
  std::unique_ptr<PageBytes> bytes;
};

/*
** A publication plan owns every allocation needed to install one transaction.
** PreparePublication constructs frames and grows the dense page table before
** WAL append. Publish consumes only already-owned objects and is noexcept.
*/
class PublicationPlan final {
 public:
  PublicationPlan(const PublicationPlan &) = delete;
  auto operator=(const PublicationPlan &) -> PublicationPlan & = delete;
  PublicationPlan(PublicationPlan &&) noexcept = default;
  auto operator=(PublicationPlan &&) noexcept -> PublicationPlan & = default;

 private:
  PublicationPlan(std::vector<std::shared_ptr<CommittedFrame>> frames, std::vector<page_id_t> retired,
                  page_id_t high_water_page_id)
      : frames_(std::move(frames)), retired_(std::move(retired)), high_water_page_id_(high_water_page_id) {}

  std::vector<std::shared_ptr<CommittedFrame>> frames_;
  std::vector<page_id_t> retired_;
  page_id_t high_water_page_id_{FIRST_DATA_PAGE_ID};

  friend class CommittedPageCache;
};

struct CommittedCacheStats {
  std::size_t target_bytes{0};
  std::size_t resident_bytes{0};
  std::size_t resident_pages{0};
  std::size_t pinned_pages{0};
  std::size_t dirty_pages{0};
  std::uint64_t checkpoint_lsn{0};
};

/*
** Thread-safe cache for latest committed versions. Its mutex protects the
** dense page table and recency clock. Pin and checkpoint flags are atomic
** because guards release outside that mutex.
*/
class CommittedPageCache final {
 public:
  CommittedPageCache(DiskManager *disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn);
  CommittedPageCache(const CommittedPageCache &) = delete;
  auto operator=(const CommittedPageCache &) -> CommittedPageCache & = delete;
  ~CommittedPageCache();

  // A miss validates the complete common page header before caching bytes.
  auto Read(page_id_t page_id) -> Result<PageGuard>;

  auto PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                          page_id_t high_water_page_id) -> Result<PublicationPlan>;
  void Publish(PublicationPlan plan) noexcept;

  // The caller invokes this only after the database file and superblock make
  // every page through checkpoint_lsn durable.
  void MarkCheckpointed(std::uint64_t checkpoint_lsn);
  void Trim();

  auto DirtyPageIds() const -> std::vector<page_id_t>;
  auto DirtyPages() -> std::vector<PageGuard>;
  auto Stats() const -> CommittedCacheStats;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cache
}  // namespace tinydb
