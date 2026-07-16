#include "verify/verifier.h"

#include "btree/b_plus_tree.h"
#include "cache/committed_page_cache.h"
#include "cache/committed_page_source.h"
#include "storage/disk_manager.h"
#include "txn/database_state.h"
#include "txn/transaction_pages.h"

#include <memory>
#include <unordered_set>
#include <utility>

namespace tinydb::verify {

/*
** SNAPSHOT OWNERSHIP AUDIT
**
** Every physical page below high_water_page_id must belong to exactly one of
** three domains:
**
**   reachable B+ tree or overflow page
**   reusable allocator extent
**   allocator metadata page
**
** TransactionPages decodes the allocator with the same persistent codec used
** by writers.  BPlusTree::CheckIntegrity then walks the disjoint ownership
** sets, validates every reachable page, and rejects both missing and duplicate
** ownership.  The transaction overlay is never edited or committed.
*/
auto Snapshot(PageReader *pages, const txn::DatabaseState &state, std::size_t memory_budget) -> Status {
  auto transaction = txn::TransactionPages::Begin(pages, state, memory_budget);
  if (!transaction) {
    return transaction.error();
  }

  auto free_pages = std::unordered_set<page_id_t>{};
  for (const auto &extent : transaction->FreeExtents()) {
    for (page_id_t page_id = extent.first_page_id; page_id < extent.first_page_id + extent.page_count; ++page_id) {
      free_pages.insert(page_id);
    }
  }
  const auto allocator_pages =
      std::unordered_set<page_id_t>(transaction->AllocatorPageIds().begin(), transaction->AllocatorPageIds().end());
  return BPlusTree::CheckIntegrity(pages, state.root_page_id, state.high_water_page_id, free_pages, allocator_pages);
}

auto CheckpointedFile(const std::filesystem::path &path, std::size_t cache_bytes,
                      std::size_t memory_budget) -> Status {
  auto opened = DiskManager::OpenReadOnly(path);
  if (!opened) {
    return opened.error();
  }
  auto disk = std::make_unique<DiskManager>(*std::move(opened));
  auto page_cache = cache::CommittedPageCache(disk.get(), cache_bytes, disk->CheckpointLsn());
  auto pages = cache::CommittedPageSource(&page_cache);
  const auto state = txn::DatabaseState{
      .root_page_id = disk->GetRootPageId(),
      .allocator_root_page_id = disk->GetAllocatorRootPageId(),
      .high_water_page_id = disk->HighWaterPageId(),
      .transaction_id = disk->TransactionId(),
      .visible_lsn = disk->CheckpointLsn(),
      .checkpoint_lsn = disk->CheckpointLsn(),
  };
  return Snapshot(&pages, state, memory_budget);
}

}  // namespace tinydb::verify
