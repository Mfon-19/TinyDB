#pragma once

#include <tinydb/file_header.h>
#include <filesystem>

namespace tinydb {

// DiskManager owns the database file descriptor and provides full-page I/O.
//
// This layer treats pages as opaque bytes. It does not know whether a page
// contains a B+ tree node, free page, overflow page, or any higher-level state.
class DiskManager {
 public:
  // Opens or creates the database file at path.
  explicit DiskManager(const std::filesystem::path &path);

  DiskManager(const DiskManager &) = delete;
  auto operator=(const DiskManager &) -> DiskManager & = delete;

  DiskManager(DiskManager &&) noexcept;
  auto operator=(DiskManager &&) noexcept -> DiskManager &;
  ~DiskManager();

  // Returns a fresh data page id. Freed-page reuse is deferred.
  auto AllocatePage() -> page_id_t;

  // The B+ tree root page id persisted in the file header. HEADER_PAGE_ID
  // means the database has no root yet (freshly created file).
  auto GetRootPageId() const -> page_id_t;

  // Records a new root page id in the file header and writes it out.
  void SetRootPageId(page_id_t root_page_id);

  // Reads exactly one page into data. Reading an unallocated page is an error.
  void ReadPage(page_id_t page_id, char *data) const;

  // Writes exactly one page from data.
  void WritePage(page_id_t page_id, const char *data) const;

 private:
  // Writes the in-memory file header to page 0.
  void WriteHeader() const;

  // fd_ == -1 means this object does not own an open file descriptor.
  int fd_{-1};
  FileHeader header_;
};
}  // namespace tinydb
