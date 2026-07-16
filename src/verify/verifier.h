#pragma once

#include <tinydb/status.h>

#include <cstddef>
#include <filesystem>

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
auto Snapshot(PageReader *pages, const txn::DatabaseState &state, std::size_t memory_budget) -> Status;

/*
** Open and verify a database file without creating a WAL, initializing an
** empty file, checkpointing, or otherwise changing persistent bytes.  This is
** the validation boundary used before a private backup image is published.
*/
auto CheckpointedFile(const std::filesystem::path &path, std::size_t cache_bytes,
                      std::size_t memory_budget) -> Status;

}  // namespace verify
}  // namespace tinydb
