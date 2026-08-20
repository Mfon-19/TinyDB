#pragma once

#include <tinydb/status.h>
#include "btree/page_source.h"
#include "cache/page_arena.h"
#include "storage/page.h"
#include "storage/page_codec.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace tinydb {

class DiskManager;

namespace cache {

struct CommittedFrame;

/*
** IMMUTABLE COMMITTED PAGE CACHE
**
** Each populated page-table entry is the newest visible committed version of
** that page, regardless of the page-file transport. Installed frame bytes
** never change. Replacing a page swaps the table's shared frame pointer,
** while an older PageHandle can continue reading the old immutable frame
** until its snapshot drains.
**
** Frames newer than checkpoint_lsn are dirty in the cache sense: WAL contains
** their durable image, but the database file does not yet. Such frames cannot
** be evicted. The byte target is soft. Publication can exceed it until
** checkpointing stores those page versions in the database file.
**
** PageHandle is the byte lease exposed to reads and checkpoints. Once a frame
** is checkpointed, the page table's ownership alone leaves it evictable; any
** additional shared owner, usually a PageHandle or an active load, pins it.
*/
/*
** TransactionPages::PrepareCommit authenticates each dirty image and attaches
** its decoded common header plus any tree-payload proof. Publication consumes
** those proofs while transferring the same page allocation into a frame.
*/
struct CommittedPageImage {
  storage::DataPageHeader header;
  PageArena::Lease bytes;
  bool tree_payload_validated{false};
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
  auto operator=(PublicationPlan &&) -> PublicationPlan & = delete;

 private:
  PublicationPlan(std::vector<std::shared_ptr<CommittedFrame>> frames, std::vector<page_id_t> retired)
      : frames_(std::move(frames)), retired_(std::move(retired)) {}

  std::vector<std::shared_ptr<CommittedFrame>> frames_;
  std::vector<page_id_t> retired_;

  friend class CommittedPageCache;
};

struct CommittedCacheStats {
  std::size_t target_bytes{0};
  std::size_t resident_bytes{0};
  std::size_t resident_pages{0};
  std::size_t pinned_pages{0};
  std::size_t dirty_pages{0};
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};
  std::uint64_t read_ahead_plans{0};
  std::uint64_t read_ahead_pages_scheduled{0};
  std::uint64_t read_ahead_pages_consumed{0};
};

/*
** Thread-safe cache for latest committed versions. Its mutex protects the
** dense page table, load-capacity reservations, and intrusive checkpointed-page
** LRU queue; physical reads and page validation run without that mutex. A
** checkpointed frame is evictable only while the table is its sole shared
** owner. PageHandle, active-load, and checkpoint references pin it. Pinned
** pages remain in the queue and eviction skips them, so a hot handle release
** needs no callback or cache lock.
**
** Demand misses are synchronous. A buffered cache owns heap-backed page
** leases; a direct cache owns aligned slab leases and may also use io_uring
** for exact read-ahead and checkpoint batches. Transport choice does not
** alter the page table, pin rules, validation, or replacement policy.
**
** One-page demand reads remain synchronous in direct mode because there is no
** batch over which to amortize reactor queueing, eventfd wakeups, and thread
** handoff. io_uring is reserved for exact multi-page advice and checkpoint
** batches, where concurrency or vectored submission can repay that overhead.
** The load table still coalesces concurrent misses for one page, preventing
** duplicate physical reads and competing frame installation.
*/
class CommittedPageCache final : public PageReader {
 public:
  CommittedPageCache(DiskManager &disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn);
  CommittedPageCache(const CommittedPageCache &) = delete;
  auto operator=(const CommittedPageCache &) -> CommittedPageCache & = delete;
  ~CommittedPageCache() override;

  auto Read(page_id_t page_id) -> Result<PageHandle> override;
  auto BeginReadStream() -> PageReadStream override;

  auto SharedPageArena() const noexcept -> std::shared_ptr<PageArena>;

  auto PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                          page_id_t logical_page_count) -> Result<PublicationPlan>;
  void Publish(PublicationPlan plan) noexcept;

  // Capture every current version that is newer than the database file. The
  // caller serializes capture with publication so these pages and the captured
  // DatabaseState describe one visibility point.
  auto CaptureDirtyPages() -> std::vector<PageHandle>;
  auto WriteCheckpointPages(std::span<const PageHandle> pages, page_id_t captured_logical_page_count) -> Status;

  void DrainIoForTesting();

  // The caller invokes this only after the database file and superblock make
  // every page through checkpoint_lsn durable.
  void MarkCheckpointed(std::uint64_t checkpoint_lsn);
  auto DirtyPages() const -> std::size_t;
  auto Stats() const -> CommittedCacheStats;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cache
}  // namespace tinydb
