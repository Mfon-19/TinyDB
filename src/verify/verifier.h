#pragma once

#include <tinydb/options.h>
#include <tinydb/stats.h>
#include <tinydb/status.h>

#include <cstddef>

namespace tinydb {

class PageReader;

namespace txn {
struct DatabaseState;
}

namespace verify {

/*
** Verify one immutable committed state.  Persistent decoding failures are
** returned as Corruption; resource and I/O failures retain their original
** status.  Verification neither repairs pages nor publishes state.
*/
auto Snapshot(PageReader *pages, const txn::DatabaseState &state, std::size_t memory_budget,
              VerifyOptions options = {}) -> Result<VerifyReport>;

}  // namespace verify
}  // namespace tinydb
