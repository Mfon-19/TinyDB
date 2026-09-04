#pragma once

/*
 * This is the interface with Linux that handles the initial
 * creation of the database file and subsequent reading and
 * writing to the file. The open descriptor holds an exclusive 
 * process lock.
 */

#include "tinydb/status.h"
#include "tinydb/storage/page.h"
#include <string_view>

namespace tinydb::storage {

class DiskManager {
public:
  static Result<DiskManager> Open(std::string_view name);

  ~DiskManager();

  DiskManager(const DiskManager &) = delete;
  DiskManager &operator=(const DiskManager &) = delete;

  DiskManager(DiskManager &&other) noexcept;
  DiskManager &operator=(DiskManager &&other) noexcept;

  [[nodiscard]] auto PageCount() const noexcept -> PageId {
    return next_page_id_;
  }

  Result<PageId> AllocatePage();
  Status WritePage(PageId page_id, const PageBytes &page);
  Status ReadPage(PageId page_id, PageBytes &page) const;
  Status Sync() const;

private:
  DiskManager(int fd, PageId next_page_id) noexcept
      : fd_(fd), next_page_id_(next_page_id) {}
  int fd_;
  PageId next_page_id_;
};

Status SyncParentDirectory(std::string_view path);
} // namespace tinydb::storage
