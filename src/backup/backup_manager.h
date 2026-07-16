#pragma once

#include <tinydb/status.h>

#include <cstddef>
#include <filesystem>

namespace tinydb {

class DiskManager;

namespace backup {

/*
** Copy an already frozen checkpoint to destination.  The destination name is
** published only after the private image is durable and passes read-only
** verification.  Existing destination files are never replaced.
*/
auto Create(const DiskManager &disk, const std::filesystem::path &destination,
            std::size_t validation_cache_bytes, std::size_t validation_memory_budget) -> Status;

}  // namespace backup
}  // namespace tinydb
