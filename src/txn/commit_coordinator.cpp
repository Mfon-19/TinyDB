#include "txn/commit_coordinator.h"

#include "wal/wal.h"

#include "btree/b_plus_tree.h"
#include "cache/committed_page_cache.h"
#include "txn/database_state.h"
#include "txn/reader_gate.h"
#include "txn/transaction_pages.h"

#include <algorithm>
#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace tinydb::txn {

auto CommitTransaction(Wal &wal, cache::CommittedPageCache &cache, ReaderGate &readers, TransactionPages &transaction,
                       BPlusTree &tree, CommitTiming *timing) -> Result<CommitInfo> {
  using Clock = std::chrono::steady_clock;
  const auto prepare_started = Clock::now();
  const auto record_prepare = [&] {
    if (timing != nullptr) {
      timing->prepare = Clock::now() - prepare_started;
    }
  };
  transaction.SetRootPageId(tree.RootPageId());
  if (!transaction.HasChanges()) {
    record_prepare();
    const auto &state = transaction.ResultingState();
    return CommitInfo{.commit_lsn = state.visible_lsn};
  }

  const auto commit_lsn = wal.NextCommitLsn();
  if (!commit_lsn) {
    record_prepare();
    transaction.Abort();
    return std::unexpected(commit_lsn.error());
  }
  if (auto status = transaction.PrepareCommit(*commit_lsn); !status.Ok()) {
    record_prepare();
    transaction.Abort();
    return std::unexpected(std::move(status));
  }

  /*
  ** PREPARE DURABLE AND VISIBLE OWNERSHIP
  **
  ** TransactionPages transfers one ordered image vector. WAL borrows and
  ** copies those bytes before the same allocations move into committed frames.
  ** The shared DatabaseState, dense page-table capacity, frame control blocks,
  ** retirement list, and encoded WAL transaction are all created while failure
  ** is still a definite abort.
  */
  const auto state = transaction.ResultingState();
  auto retired = std::vector<page_id_t>(transaction.RetiredPageIds().begin(), transaction.RetiredPageIds().end());
  std::ranges::sort(retired);
  auto committed_pages = transaction.TakePages();
  auto wal_pages = std::vector<wal_format::PageImageView>{};
  wal_pages.reserve(committed_pages.size());
  for (const auto &image : committed_pages) {
    wal_pages.push_back(wal_format::PageImageView{
        .page_id = image.header.page_id,
        .bytes = std::span<const char, PAGE_SIZE>{image.bytes->data(), PAGE_SIZE},
        .validated_header = &image.header,
    });
  }
  auto prepared_wal = wal.Prepare(wal_pages, state);
  if (!prepared_wal) {
    record_prepare();
    transaction.Abort();
    return std::unexpected(prepared_wal.error());
  }
  auto publication_plan =
      cache.PreparePublication(std::move(committed_pages), std::move(retired), state.logical_page_count);
  if (!publication_plan) {
    record_prepare();
    transaction.Abort();
    return std::unexpected(publication_plan.error());
  }
  // This object remains private through WAL synchronization.
  auto published_state = std::make_shared<DatabaseState>(state);
  record_prepare();

  const auto wal_started = Clock::now();
  const auto durable = wal.Commit(std::move(*prepared_wal));
  if (timing != nullptr) {
    timing->wal_sync = Clock::now() - wal_started;
  }
  if (!durable) {
    transaction.Abort();
    return std::unexpected(durable.error());
  }
  /*
  ** INFALLIBLE PUBLICATION
  **
  ** BeginPublication may wait for old readers, but publication itself only
  ** consumes prepared ownership. No allocation, validation, maintenance, or
  ** I/O remains after WAL durability.
  */
  {
    const auto publication_started = Clock::now();
    auto publication = readers.BeginPublication();
    if (timing != nullptr) {
      timing->publication_wait = Clock::now() - publication_started;
    }
    cache.Publish(std::move(*publication_plan));
    publication.Publish(std::move(published_state));
  }
  return CommitInfo{.commit_lsn = *durable};
}

}  // namespace tinydb::txn
