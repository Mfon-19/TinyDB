#pragma once

#include <tinydb/options.h>
#include <tinydb/stats.h>
#include <tinydb/status.h>

#include "storage/page_codec.h"

#include <cstddef>
#include <vector>

namespace tinydb {

class PageReader;

namespace txn {
struct DatabaseState;
}

namespace verify {

struct SnapshotReport final {
  VerifyReport report;
  std::vector<storage::FreeExtent> free_extents;
};

/*
** Verify one immutable committed state.  Persistent decoding failures are
** returned as Corruption; resource and I/O failures retain their original
** status.  Verification neither repairs pages nor publishes state.
*/
auto Snapshot(PageReader *pages, const txn::DatabaseState &state, std::size_t memory_budget,
              VerifyOptions options = {}) -> Result<SnapshotReport>;

auto StatusFrom(const SnapshotReport &verified) -> Status;

}  // namespace verify
}  // namespace tinydb
