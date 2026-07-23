#pragma once

#include <tinydb/status.h>

#include <filesystem>

namespace tinydb::recovery {

/*
** Restore the database file through the latest complete durable WAL
** transaction.  The caller must already hold exclusive process ownership.
** Success means the database file and its selected superblock cover every
** accepted transaction and the remaining WAL contains only a clean header.
**
** Corruption and unsupported input are reported before database pages are
** written.  Environmental failures during redo leave the previous
** superblock and complete WAL sufficient to repeat recovery.
*/
auto Recover(const std::filesystem::path &db_path, const std::filesystem::path &wal_path) -> Status;

}  // namespace tinydb::recovery
