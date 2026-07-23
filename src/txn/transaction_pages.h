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
** The first Edit copies a committed page once; later edits reuse the same
** stable heap-owned frame. Allocate reserves either a checkpoint-safe free ID
** or the private high-water frontier. Free removes a private image and records
** a retirement without changing committed allocator state.
**
** Nothing owned here is reachable through the committed cache. Therefore
** abort requires no undo: dropping the overlay restores the base state.
** PrepareCommit finishes allocator work and seals every final page once.
**
** Memory accounting covers private pages and retained value bytes. All page
** handles must be released before PrepareCommit, Abort, ownership transfer, or
** destruction, because those operations may invalidate frame ownership.
*/
class TransactionPages final : public PageSource {
 public:
  static auto Begin(PageReader *committed, DatabaseState base_state,
                    std::size_t memory_limit_bytes) -> Result<TransactionPages>;

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

  // Finish allocator metadata, assign one commit LSN, and seal every final
  // private image. The WAL LSN no longer depends on page count, so structural
  // preparation and byte sealing are one operation.
  auto PrepareCommit(std::uint64_t commit_lsn) -> Status;
  void Abort() noexcept;

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
    page_id_t page_id{HEADER_PAGE_ID};        // immutable identity of this frame
    std::unique_ptr<cache::PageBytes> bytes;  // stable address until transfer
    std::size_t pin_count{0};                 // outstanding PageHandles
    bool editing{false};                      // mutable leases are exclusive
    bool dirty{false};                        // needs WAL/publication image
    bool tree_payload_validated{false};       // trusted private tree bytes
    storage::DataPageHeader sealed_header{};  // page_id zero means not sealed
  };

  TransactionPages(PageReader *committed, DatabaseState base_state, std::size_t memory_limit_bytes)
      : committed_(committed),
        base_state_(base_state),
        resulting_state_(base_state),
        memory_limit_bytes_(memory_limit_bytes) {}

  static void ReleasePrivate(void *owner, page_id_t page_id, bool dirty, bool tree_payload_validated);
  static auto PrivateHandle(PrivateFrame *frame, bool editable) -> PageHandle;
  auto CreatePrivatePage(page_id_t page_id, bool dirty) -> Result<PrivateFrame *>;
  auto AllocateHighWaterPage() -> Result<PrivateFrame *>;
  auto LoadFreeExtents() -> Status;
  auto AllocateReusablePage() -> std::optional<page_id_t>;
  void AddRetiredExtents(std::uint64_t retire_lsn);
  auto EnsureFreeExtentIndexFrames() -> Status;
  auto StoreFreeExtentIndex() -> Status;
  auto ChargePage() -> Status;
  void RequireActive() const;
  void RequireUnpinned() const;

  PageReader *committed_;           // immutable fallback for reads
  DatabaseState base_state_;        // captured at Begin
  DatabaseState resulting_state_;   // private roots/frontiers
  std::size_t memory_limit_bytes_;  // pages plus retained values
  std::size_t memory_used_bytes_{0};
  bool prepared_{false};         // final page set carries the exact commit LSN
  bool aborted_{false};          // private state was discarded
  bool allocator_dirty_{false};  // extent index needs rewriting

  std::unordered_map<page_id_t, std::unique_ptr<PrivateFrame>> pages_;
  std::unordered_set<page_id_t> retired_page_ids_;
  std::vector<storage::FreeExtent> free_extents_;
  std::vector<page_id_t> allocator_page_ids_;
};

}  // namespace tinydb::txn
