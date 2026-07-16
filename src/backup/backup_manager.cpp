#include "backup/backup_manager.h"

#include "io/file_io.h"
#include "io/syscalls.h"
#include "io/unique_fd.h"
#include "storage/disk_manager.h"
#include "verify/verifier.h"

#include <fcntl.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

namespace tinydb::backup {
namespace {

/*
** BACKUP FILE PUBLICATION
**
** The source database is already checkpointed and physically frozen by the
** caller.  Backup writes a UUID-named sibling staging file so a crash cannot
** expose a partial destination:
**
**   create staging -> copy selected checkpoint -> fsync staging
**   -> verify staging read-only -> link staging to destination
**   -> unlink staging -> fsync parent directory
**
** link(2) is the publication point.  It atomically refuses an existing
** destination rather than replacing application data.  Before that point any
** environmental or corruption error leaves only a private staging name, which
** StagingFile removes.  A process crash may leave that private name; the next
** attempt for the same database UUID removes it before beginning.
*/
auto UuidSuffix(const DatabaseUuid &uuid) -> std::string {
  constexpr auto digits = std::array{'0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  auto result = std::string{};
  result.reserve(uuid.size() * 2U);
  for (const auto byte : uuid) {
    const auto value = std::to_integer<unsigned char>(byte);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

auto StagingPath(const std::filesystem::path &destination, const DatabaseUuid &uuid) -> std::filesystem::path {
  auto staging = destination;
  staging += ".tinydb-" + UuidSuffix(uuid) + ".tmp";
  return staging;
}

auto RemoveIfPresent(const std::filesystem::path &path) -> Status {
  if (io::Unlink(path) == 0 || errno == ENOENT) {
    return {};
  }
  return io::ErrnoStatus("unlink backup staging file");
}

class StagingFile final {
 public:
  explicit StagingFile(std::filesystem::path path) : path_(std::move(path)) {}
  StagingFile(const StagingFile &) = delete;
  auto operator=(const StagingFile &) -> StagingFile & = delete;
  ~StagingFile() {
    if (remove_) {
      static_cast<void>(RemoveIfPresent(path_));
    }
  }

  auto Path() const -> const std::filesystem::path & { return path_; }
  void Published() noexcept { remove_ = false; }

 private:
  std::filesystem::path path_;
  bool remove_{true};
};

}  // namespace

auto Create(const DiskManager &disk, const std::filesystem::path &destination,
            std::size_t validation_cache_bytes, std::size_t validation_memory_budget) -> Status {
  if (destination.empty()) {
    return Status::InvalidArgument("backup destination path is empty");
  }

  auto staging = StagingFile(StagingPath(destination, disk.Uuid()));
  if (auto status = RemoveIfPresent(staging.Path()); !status.Ok()) {
    return status;
  }

  {
    auto output = UniqueFd(io::Open(staging.Path(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (!output.Valid()) {
      return io::ErrnoStatus("open backup staging file");
    }
    if (auto status = disk.CopyCheckpointTo(output.Get()); !status.Ok()) {
      return status;
    }
  }

  if (auto status = verify::CheckpointedFile(staging.Path(), validation_cache_bytes, validation_memory_budget);
      !status.Ok()) {
    return status;
  }

  if (io::Link(staging.Path(), destination) < 0) {
    return errno == EEXIST ? Status::InvalidArgument("backup destination already exists")
                          : io::ErrnoStatus("publish backup");
  }
  if (io::Unlink(staging.Path()) < 0) {
    return io::ErrnoStatus("unlink published backup staging file");
  }
  staging.Published();
  return io::SyncParentDirectory(destination);
}

}  // namespace tinydb::backup
