#pragma once

#include <tinydb/status.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace tinydb::io {

/*
** COMPLETE FILE OPERATIONS
**
** POSIX pread and pwrite may complete only part of a request and may be
** interrupted before transferring bytes.  Persistent protocols cannot treat
** either condition as a complete page or record.
**
** Buffered database pages and every WAL operation use these helpers. Direct
** database pages use DirectFile instead, because each retry must preserve
** O_DIRECT memory, offset, and length alignment.
**
** FullPread reports clean end-of-file by returning the short byte count.
** FullPwrite reports the first environmental failure and otherwise transfers
** the complete range.  SyncParentDirectory is the durability operation for a
** newly created, renamed, or removed directory entry.
*/

auto ErrnoStatus(std::string_view operation) -> Status;
auto FullPread(int fd, void *data, std::size_t size, std::uint64_t offset) -> Result<std::size_t>;
auto FullPwrite(int fd, const void *data, std::size_t size, std::uint64_t offset) -> Status;
auto SyncParentDirectory(const std::filesystem::path &path) -> Status;

}  // namespace tinydb::io
