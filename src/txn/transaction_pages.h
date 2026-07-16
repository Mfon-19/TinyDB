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

// Private copy-on-write page context for one write transaction. Nothing in
// this object is reachable through the committed cache until its frozen page
// images are handed to the later commit coordinator.
class TransactionPages final : public PageSource {
 public:
  static auto Begin(PageReader *committed, DatabaseState base_state,
                    std::size_t memory_limit_bytes) -> Result<TransactionPages>;

  TransactionPages(const TransactionPages &) = delete;
  auto operator=(const TransactionPages &) -> TransactionPages & = delete;
  TransactionPages(TransactionPages &&) noexcept = default;
  auto operator=(TransactionPages &&) noexcept -> TransactionPages & = default;
  ~TransactionPages();

  auto Read(page_id_t page_id) -> Result<PageHandle> override;
  auto Edit(page_id_t page_id) -> Result<PageHandle> override;
  auto Allocate() -> Result<PageHandle> override;
  auto Free(page_id_t page_id) -> Status override;

  void SetRootPageId(page_id_t root_page_id);
  auto ChargeValueBytes(std::size_t bytes) -> Status;

  // Finalizes retirements and the persistent free-extent index. retire_lsn is
  // assigned by the caller's current commit path; Milestone 6 will replace it
  // with the commit coordinator's global LSN.
  auto Freeze(std::uint64_t retire_lsn) -> Status;
  void Abort() noexcept;

  auto ResultingState() const -> const DatabaseState &;
  auto MemoryUsedBytes() const -> std::size_t { return memory_used_bytes_; }
  auto MemoryLimitBytes() const -> std::size_t { return memory_limit_bytes_; }
  auto PrivatePageCount() const -> std::size_t { return pages_.size(); }
  auto HasChanges() const -> bool;
  auto RetiredPageIds() const -> const std::unordered_set<page_id_t> & { return retired_page_ids_; }
  auto FreeExtents() const -> const std::vector<storage::FreeExtent> & { return free_extents_; }
  auto AllocatorPageIds() const -> const std::vector<page_id_t> & { return allocator_page_ids_; }

  // Borrowed images remain stable until Abort, destruction, or TakePages.
  auto PageImages() const -> std::vector<std::pair<page_id_t, const char *>>;
  auto TakePages(std::uint64_t transaction_id) -> Result<std::vector<cache::CommittedPageImage>>;

 private:
  struct PrivateFrame {
    page_id_t page_id{HEADER_PAGE_ID};
    std::unique_ptr<cache::PageBytes> bytes;
    std::size_t pin_count{0};
    bool dirty{false};
  };

  TransactionPages(PageReader *committed, DatabaseState base_state, std::size_t memory_limit_bytes)
      : committed_(committed),
        base_state_(base_state),
        resulting_state_(base_state),
        memory_limit_bytes_(memory_limit_bytes) {}

  static void ReleasePrivate(void *owner, page_id_t page_id, bool dirty);
  auto PrivateHandle(PrivateFrame *frame, bool editable) -> PageHandle;
  auto CreatePrivatePage(page_id_t page_id, bool dirty) -> Result<PrivateFrame *>;
  auto AllocateHighWaterPage() -> Result<PrivateFrame *>;
  auto LoadFreeExtents() -> Status;
  auto AllocateReusablePage() -> std::optional<page_id_t>;
  void AddRetiredExtents(std::uint64_t retire_lsn);
  auto StoreFreeExtentIndex(std::uint64_t page_lsn) -> Status;
  auto ChargePage() -> Status;
  void RequireActive() const;
  void RequireUnpinned() const;

  PageReader *committed_;
  DatabaseState base_state_;
  DatabaseState resulting_state_;
  std::size_t memory_limit_bytes_;
  std::size_t memory_used_bytes_{0};
  bool frozen_{false};
  bool aborted_{false};
  bool allocator_dirty_{false};

  std::unordered_map<page_id_t, std::unique_ptr<PrivateFrame>> pages_;
  std::unordered_set<page_id_t> retired_page_ids_;
  std::vector<storage::FreeExtent> free_extents_;
  std::vector<page_id_t> allocator_page_ids_;
};

}  // namespace tinydb::txn
