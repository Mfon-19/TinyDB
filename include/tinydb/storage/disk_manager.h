#pragma once

/*
 * This is the interface with Linux that handles the initial
 * creation of the database file and subsequent reading and
 * writing to the file.
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

  Status WritePage(PageId page_id, const PageBytes &page);
  Status ReadPage(PageId page_id, PageBytes &page) const;

private:
  explicit DiskManager(int fd) noexcept : fd_(fd) {}
  int fd_; // the file descriptor we get from Linux
};
} // namespace tinydb::storage
