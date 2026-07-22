#pragma once

#include <tinydb/status.h>
#include <tinydb/transaction.h>

#include <chrono>

namespace tinydb {

class BPlusTree;
class Wal;

namespace cache {
class CommittedPageCache;
}

namespace txn {

class ReaderGate;
class TransactionPages;

struct CommitTiming final {
  std::chrono::nanoseconds prepare{};
  std::chrono::nanoseconds wal_sync{};
  std::chrono::nanoseconds publication_wait{};
};

/*
** DURABLE COMMIT
**
** Join one frozen logical transaction to WAL durability, committed-cache
** ownership, and visible DatabaseState. The operation owns none of those
** layers and performs no tree mutation itself.
**
** Commit establishes the following boundary:
**
**   before WAL fsync   validation/allocation may fail; state is discardable
**   after WAL fsync    only prepared ownership transfer and scalar publication
**
** An IoError returned after successful append-tail repair is a definite abort.
** IndeterminateCommit or NeedsRecovery means the caller must transition the
** database lifecycle before admitting another operation.
*/
auto CommitTransaction(Wal &wal, cache::CommittedPageCache &cache, ReaderGate &readers, TransactionPages &transaction,
                       BPlusTree &tree, CommitTiming *timing = nullptr) -> Result<CommitInfo>;

}  // namespace txn
}  // namespace tinydb
