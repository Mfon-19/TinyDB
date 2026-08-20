#pragma once

#include <tinydb/status.h>

#include <filesystem>

namespace tinydb::io {
class PageFile;
}

namespace tinydb::recovery {

/*
** Restore the database file through the latest complete durable WAL
** transaction. The caller must already hold exclusive process ownership.
** Success means that the database file and selected superblock cover every
** accepted transaction and that no WAL transaction remains pending for redo.
** Wal::Open creates or checks the clean header afterward. Buffered and direct
** page transports produce the same persistent bytes and durability order.
**
** Corruption and unsupported input are reported before database pages are
** written.  Environmental failures during redo leave the previous
** superblock and complete WAL sufficient to repeat recovery.
*/
auto Recover(io::PageFile &database, const std::filesystem::path &wal_path) -> Status;

}  // namespace tinydb::recovery
