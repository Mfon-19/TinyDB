#include "tinydb/storage/disk_manager.h"
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <utility>

namespace tinydb::storage {
namespace {

auto PageOffset(PageId page_id) noexcept -> off_t {
  return static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
}

} // namespace

auto DiskManager::Open(std::string_view name) -> Result<DiskManager> {
  auto file = detail::File::Open(name, O_CREAT | O_RDWR);
  if (!file) {
    return Err(std::move(file.error()));
  }
  if (flock(file->Get(), LOCK_EX | LOCK_NB) == -1) {
    const int error = errno;
    if (error == EWOULDBLOCK) {
      return Err(Status::ResourceExhausted("database is already open"));
    }
    return Err(detail::SystemError("failed to lock database", error));
  }
  struct stat info {};
  if (fstat(file->Get(), &info) == -1) {
    return Err(detail::SystemError("failed to inspect database", errno));
  }
  if (info.st_nlink != 1) {
    return Err(
        Status::InvalidArgument("database must have exactly one hard link"));
  }
  return DiskManager(std::move(*file));
}

auto DiskManager::PageCount() const -> Result<PageId> {
  auto size = file_.Size();
  if (!size) {
    return Err(std::move(size.error()));
  }
  const auto pages =
      (static_cast<std::uint64_t>(*size) + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages >= INVALID_PAGE_ID) {
    return Err(Status::ResourceExhausted("database has too many pages"));
  }
  return static_cast<PageId>(pages);
}

auto DiskManager::Sync() const -> Status { return file_.Sync(); }

auto SyncParentDirectory(std::string_view path) -> Status {
  const auto slash = path.find_last_of('/');
  const auto parent = slash == std::string_view::npos
                          ? std::string_view{"."}
                          : path.substr(0, slash == 0 ? 1 : slash);
  auto directory = detail::File::Open(parent, O_RDONLY | O_DIRECTORY);
  if (!directory) {
    return std::move(directory.error());
  }
  return directory->Sync();
}

auto DiskManager::WritePage(PageId page_id, const PageBytes &page) -> Status {
  return file_.Write(PageOffset(page_id), page);
}

auto DiskManager::ReadPage(PageId page_id, PageBytes &page) const -> Status {
  return file_.Read(PageOffset(page_id), page);
}

} // namespace tinydb::storage
