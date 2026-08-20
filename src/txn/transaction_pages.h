#pragma once

#include "btree/page_source.h"
#include "cache/committed_page_cache.h"
#include "storage/page_codec.h"
#include "txn/database_state.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb::txn {

/*
** TRANSACTION-LOCAL PAGE OVERLAY
**
** TransactionPages is the complete mutable page universe of one writer. A
** read first consults the private map and otherwise borrows committed bytes.
** The first Edit copies a committed page once; later edits reuse the stable
** private frame. Allocate reuses a checkpoint-safe page ID when possible and
** otherwise extends the private logical page count. Free removes a private
** image and records a retirement without changing committed allocator state.
**
** Nothing owned here is reachable through the committed cache. Therefore
** abort requires no undo: dropping the overlay restores the base state.
** PrepareCommit finishes allocator work and seals every dirty final page.
** Edits retain their logical page IDs; after reader quiescence, publication
** replaces the committed frame for that ID. This is page shadowing inside one
** transaction, not an append-only physical version chain.
**
** Memory accounting charges private pages and each value accepted by Put.
** Value charges remain until the transaction ends, even if a later mutation
** replaces or deletes that value. All page handles must be released before
** PrepareCommit, Abort, ownership transfer, or destruction because those
** operations can invalidate frame ownership.
**
** The committed cache supplies the PageArena: heap-backed for buffered I/O or
** aligned and slab-backed for direct I/O. Copy-on-write and commit are
** otherwise identical, and publication transfers leases without changing
** their backing storage.
*/
class TransactionPages final : public PageSource {
 public:
  static auto Begin(PageReader *committed, DatabaseState base_state, std::size_t memory_limit_bytes,
                    std::shared_ptr<cache::PageArena> page_arena = {}) -> Result<TransactionPages>;

  TransactionPages(const TransactionPages &) = delete;
  auto operator=(const TransactionPages &) -> TransactionPages & = delete;
  TransactionPages(TransactionPages &&) noexcept = default;
  auto operator=(TransactionPages &&) -> TransactionPages & = delete;
  ~TransactionPages() override;

  auto Read(page_id_t page_id) -> Result<PageHandle> override;
  auto Edit(page_id_t page_id) -> Result<PageHandle> override;
  auto Allocate() -> Result<PageHandle> override;
  auto Free(page_id_t page_id) -> Status override;

  void SetRootPageId(page_id_t root_page_id);
  auto ChargeValueBytes(std::size_t bytes) -> Status;

  // Finish allocator metadata, assign the commit LSN, and seal each dirty
  // private image before its lease is transferred to WAL and cache preparation.
  auto PrepareCommit(std::uint64_t commit_lsn) -> Status;

  auto ResultingState() const -> const DatabaseState &;
  auto HasChanges() const -> bool;
  auto RetiredPageIds() const -> const std::unordered_set<page_id_t> & { return retired_page_ids_; }
  auto FreeExtents() const -> const std::vector<storage::FreeExtent> & { return free_extents_; }
  auto AllocatorPageIds() const -> const std::vector<page_id_t> & { return allocator_page_ids_; }

  // Transfer final images in page-ID order so WAL encoding and publication
  // share one deterministic representation without copying page bytes.
  auto TakePages() -> std::vector<cache::CommittedPageImage>;

 private:
  struct PrivateFrame {
    page_id_t page_id{HEADER_PAGE_ID};
    cache::PageArena::Lease bytes;
    std::size_t pin_count{0};
    bool editing{false};
    bool dirty{false};
    PagePayloadProof payload_proof{PagePayloadProof::None};
    storage::DataPageHeader sealed_header{};
  };

  TransactionPages(PageReader *committed, DatabaseState base_state, std::size_t memory_limit_bytes,
                   std::shared_ptr<cache::PageArena> page_arena)
      : committed_(committed),
        base_state_(base_state),
        resulting_state_(base_state),
        memory_limit_bytes_(memory_limit_bytes),
        page_arena_(std::move(page_arena)) {}

  static void ReleasePrivate(void *owner, page_id_t page_id, bool dirty, PagePayloadProof payload_proof);
  static auto PrivateHandle(PrivateFrame *frame, bool editable) -> PageHandle;
  auto CreatePrivatePage(page_id_t page_id, bool dirty) -> Result<PrivateFrame *>;
  auto AllocateNewPage() -> Result<PrivateFrame *>;
  auto LoadFreeExtents() -> Status;
  auto AllocateReusablePage() -> std::optional<page_id_t>;
  void AddRetiredExtents(std::uint64_t retire_lsn);
  auto EnsureFreeExtentIndexFrames() -> Status;
  auto StoreFreeExtentIndex() -> Status;
  auto Charge(std::size_t bytes) -> Status;
  void RequireActive() const;
  void RequireUnpinned() const;

  PageReader *committed_;
  DatabaseState base_state_;
  DatabaseState resulting_state_;
  std::size_t memory_limit_bytes_;
  std::shared_ptr<cache::PageArena> page_arena_;
  std::size_t memory_used_bytes_{0};
  bool prepared_{false};
  bool allocator_dirty_{false};

  // unordered_map preserves element addresses across rehash. Free removes a
  // frame only after proving that no PageHandle still refers to it.
  std::unordered_map<page_id_t, PrivateFrame> pages_;
  std::unordered_set<page_id_t> retired_page_ids_;
  std::vector<storage::FreeExtent> free_extents_;
  std::vector<page_id_t> allocator_page_ids_;
};

}  // namespace tinydb::txn
