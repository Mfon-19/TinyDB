#pragma once

#include <tinydb/options.h>
#include <tinydb/stats.h>
#include <tinydb/status.h>

namespace tinydb {

class PageReader;

namespace txn {
struct DatabaseState;
}

namespace verify {

/*
** Verify one immutable committed state. Persistent decoding failures become
** findings in the returned report. Other failures returned by the page reader
** retain their original Status. Verification neither repairs pages nor
** publishes state.
*/
auto Snapshot(PageReader *pages, const txn::DatabaseState &state, VerifyOptions options = {}) -> Result<VerifyReport>;

}  // namespace verify
}  // namespace tinydb
