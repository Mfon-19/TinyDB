#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tinydb {

class DiskManager;
class PageHandle;

namespace cache {

using PageBytes = std::array<char, PAGE_SIZE>;

struct CommittedFrame;

// A guard is the only way cache clients retain page bytes. Guards are
// move-only so one cache pin has one obvious owner and one release point.
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

// A write transaction eventually transfers these bytes directly into a
// committed frame. unique_ptr makes the no-copy ownership transfer explicit.
struct CommittedPageImage {
  page_id_t page_id{HEADER_PAGE_ID};
  std::uint64_t page_lsn{0};
  std::uint64_t transaction_id{0};
  std::unique_ptr<PageBytes> bytes;
};

struct CommittedCacheStats {
  std::size_t target_bytes{0};
  std::size_t resident_bytes{0};
  std::size_t resident_pages{0};
  std::size_t pinned_pages{0};
  std::size_t dirty_pages{0};
  std::uint64_t checkpoint_lsn{0};
};

// Thread-safe cache for the latest committed page versions. Page bytes are
// immutable after installation; synchronization protects only the page table,
// LRU metadata, pin accounting, and checkpoint eligibility.
class CommittedPageCache final {
 public:
  CommittedPageCache(DiskManager *disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn);
  CommittedPageCache(const CommittedPageCache &) = delete;
  auto operator=(const CommittedPageCache &) -> CommittedPageCache & = delete;
  ~CommittedPageCache();

  // A miss validates the complete common page header before caching bytes.
  auto Read(page_id_t page_id) -> Result<PageGuard>;

  // Installs the latest committed version. The cache may exceed its soft target
  // while uncheckpointed pages are not eligible for eviction.
  auto Install(CommittedPageImage image) -> Status;

  // The caller invokes this only after the database file and superblock make
  // every page through checkpoint_lsn durable.
  void MarkCheckpointed(std::uint64_t checkpoint_lsn);
  void Trim();

  auto DirtyPageIds() const -> std::vector<page_id_t>;
  auto Stats() const -> CommittedCacheStats;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cache
}  // namespace tinydb
