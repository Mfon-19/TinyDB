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
** DURABLE COMMIT COORDINATOR
**
** This class joins one frozen logical transaction to the three shared storage
** domains it must update: WAL durability, committed-cache ownership, and
** visible DatabaseState. It owns none of those layers and performs no tree
** mutation itself.
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
class CommitCoordinator final {
 public:
  CommitCoordinator(Wal *wal, cache::CommittedPageCache *cache, ReaderGate *readers)
      : wal_(wal), cache_(cache), readers_(readers) {}

  auto Commit(TransactionPages &transaction, BPlusTree &tree, CommitTiming *timing = nullptr) -> Result<CommitInfo>;

 private:
  Wal *wal_;
  cache::CommittedPageCache *cache_;
  ReaderGate *readers_;
};

}  // namespace txn
}  // namespace tinydb
