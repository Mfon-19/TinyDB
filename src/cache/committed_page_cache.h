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
** The page table contains only the newest visible committed version of each
** page. Installed frame bytes never change. Replacing a page swaps the table's
** shared frame pointer, while an older PageHandle can continue reading the old
** immutable frame until its snapshot drains.
**
** Frames newer than checkpoint_lsn are dirty in the cache sense: WAL contains
** their durable image, but the database file does not yet. Such frames cannot
** be evicted. The byte target is soft. Publication can exceed it until
** checkpointing stores those page versions in the database file.
**
** PageHandle is the sole byte lease for reads and checkpoints. Its shared
** keepalive both retains the immutable frame and supplies the exact cache pin.
*/
/*
** Seal has authenticated the final bytes and attaches their decoded common
** header plus the builder's tree-payload proof. Publication consumes those
** proofs while transferring the same allocation directly into a frame.
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
** frame's shared ownership is also its exact pin count: the table owns one
** reference and each PageHandle owns another. Pinned pages remain in the queue
** and eviction skips them, so a hot handle release needs no callback or cache
** lock.
*/
class CommittedPageCache final : public PageReader {
 public:
  CommittedPageCache(DiskManager &disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn);
  CommittedPageCache(const CommittedPageCache &) = delete;
  auto operator=(const CommittedPageCache &) -> CommittedPageCache & = delete;
  ~CommittedPageCache() override;

  // A miss validates the complete common page header before caching bytes.
  auto Read(page_id_t page_id) -> Result<PageHandle> override;
  // When the native engine is available, a direct stream accepts exact-page
  // plans. Buffered and unavailable engines use the ordinary read path.
  auto BeginReadStream() -> PageReadStream override;

  // Write transactions use this same arena. Commit can therefore transfer an
  // aligned direct page into the cache without a page-sized copy.
  auto SharedPageArena() const noexcept -> std::shared_ptr<PageArena>;

  auto PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                          page_id_t logical_page_count) -> Result<PublicationPlan>;
  void Publish(PublicationPlan plan) noexcept;

  // Capture every current version that is newer than the database file. The
  // caller serializes capture with publication so these pages and its captured
  // DatabaseState describe one visibility point.
  auto CaptureDirtyPages() -> std::vector<PageHandle>;
  // Direct mode first uses the native checkpoint queue. If no native write
  // starts, the method writes the complete page set synchronously.
  auto WriteCheckpointPages(std::span<const PageHandle> pages, page_id_t captured_logical_page_count) -> Status;

  // Wait until the native direct-I/O queue has no queued or active work.
  // Buffered caches have no engine, so this returns immediately.
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
