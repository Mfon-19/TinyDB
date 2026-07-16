#include "txn/commit_coordinator.h"

#include "util/check.h"
#include "wal/wal.h"

#include "btree/b_plus_tree.h"
#include "cache/committed_page_cache.h"
#include "txn/database_state.h"
#include "txn/reader_gate.h"
#include "txn/transaction_pages.h"

#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace tinydb::txn {

auto CommitCoordinator::Commit(TransactionPages &transaction, BPlusTree &tree,
                               TransactionState &transaction_state) -> Result<CommitInfo> {
  transaction.SetRootPageId(tree.RootPageId());
  if (auto status = transaction.Freeze(); !status.Ok()) {
    transaction.Abort();
    transaction_state = TransactionState::Aborted;
    return std::unexpected(std::move(status));
  }
  transaction_state = TransactionState::Frozen;
  if (!transaction.HasChanges()) {
    const auto &state = transaction.ResultingState();
    transaction_state = TransactionState::Published;
    return CommitInfo{.transaction_id = state.transaction_id, .commit_lsn = state.visible_lsn};
  }

  const auto commit_lsn = wal_->NextCommitLsn(transaction.FinalPageCount());
  if (!commit_lsn) {
    transaction.Abort();
    transaction_state = TransactionState::Aborted;
    return std::unexpected(commit_lsn.error());
  }
  if (auto status = transaction.Seal(*commit_lsn); !status.Ok()) {
    transaction.Abort();
    transaction_state = TransactionState::Aborted;
    return std::unexpected(std::move(status));
  }

  /*
  ** PREPARE DURABLE AND VISIBLE OWNERSHIP
  **
  ** WAL copies final bytes before TransactionPages transfers them into
  ** committed frames. The shared DatabaseState, dense page-table capacity,
  ** frame control blocks, retirement list, and encoded WAL transaction are all
  ** created while failure is still a definite abort.
  */
  const auto borrowed_images = transaction.PageImages();
  for (const auto &[page_id, bytes] : borrowed_images) {
    wal_->AppendPageImage(page_id, bytes);
  }
  auto state = transaction.ResultingState();
  auto retired = std::vector<page_id_t>(transaction.RetiredPageIds().begin(), transaction.RetiredPageIds().end());
  auto committed_pages = transaction.TakePages(state.transaction_id);
  if (!committed_pages) {
    wal_->DiscardPending();
    transaction.Abort();
    transaction_state = TransactionState::Aborted;
    return std::unexpected(committed_pages.error());
  }
  auto publication_plan =
      cache_->PreparePublication(std::move(*committed_pages), std::move(retired), state.high_water_page_id);
  if (!publication_plan) {
    wal_->DiscardPending();
    transaction.Abort();
    transaction_state = TransactionState::Aborted;
    return std::unexpected(publication_plan.error());
  }
  // This object remains private through WAL synchronization. Publication may
  // raise its checkpoint frontier to one completed concurrently, which is a
  // no-throw scalar update and does not change the WAL-authenticated mutation.
  auto published_state = std::make_shared<DatabaseState>(state);

  transaction_state = TransactionState::WritingWal;
  const auto durable = wal_->Commit(state);
  if (!durable) {
    wal_->DiscardPending();
    transaction.Abort();
    transaction_state =
        durable.error().Code() == StatusCode::IndeterminateCommit || durable.error().Code() == StatusCode::NeedsRecovery
            ? TransactionState::Indeterminate
            : TransactionState::Aborted;
    return std::unexpected(durable.error());
  }
  transaction_state = TransactionState::Durable;
  TINYDB_CHECK(durable->transaction_id == state.transaction_id && durable->commit_lsn == state.visible_lsn,
               "WAL committed a different frozen transaction");

  /*
  ** INFALLIBLE PUBLICATION
  **
  ** BeginPublication may wait for old readers, but publication itself only
  ** consumes prepared ownership. No allocation, validation, maintenance, or
  ** I/O remains after WAL durability.
  */
  {
    auto publication = readers_->BeginPublication();
    published_state->checkpoint_lsn =
        std::max(published_state->checkpoint_lsn, publication.CurrentState()->checkpoint_lsn);
    cache_->Publish(std::move(*publication_plan));
    publication.Publish(std::move(published_state));
  }
  transaction_state = TransactionState::Published;
  return CommitInfo{.transaction_id = durable->transaction_id, .commit_lsn = durable->commit_lsn};
}

}  // namespace tinydb::txn
