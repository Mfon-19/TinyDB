#pragma once

#include <tinydb/file_header.h>
#include <tinydb/unique_fd.h>

#include <filesystem>
#include <unordered_set>

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

  // UniqueFd carries the descriptor across moves and closes it on
  // destruction, so the compiler-generated special members are correct.
  DiskManager(DiskManager &&) noexcept = default;
  auto operator=(DiskManager &&) noexcept -> DiskManager & = default;
  ~DiskManager() = default;

  // Returns a data page id: the most recently freed page if any, otherwise
  // a fresh page grown at the end of the file. The page's on-disk bytes are
  // unspecified; the caller writes it before reading it.
  auto AllocatePage() -> page_id_t;

  // Puts page_id on the free list for AllocatePage to reuse. The caller
  // must own the page and drop every reference to it first. Freeing a page
  // twice is a bug and aborts.
  void FreePage(page_id_t page_id);

  // The B+ tree root page id persisted in the file header. HEADER_PAGE_ID
  // means the database has no root yet (freshly created file).
  auto GetRootPageId() const -> page_id_t;

  // Records a new root page id in the file header and writes it out.
  void SetRootPageId(page_id_t root_page_id);

  // Blocks until every write so far has reached the storage device, not just
  // the OS page cache. This is the durability point for a clean close.
  void Sync() const;

  // Reads exactly one page into data. Reading an unallocated page is an error.
  void ReadPage(page_id_t page_id, char *data) const;

  // Writes exactly one page from data.
  void WritePage(page_id_t page_id, const char *data) const;

 private:
  // Writes the in-memory file header to page 0.
  void WriteHeader() const;

  // An invalid fd means this object was moved from.
  UniqueFd fd_;
  FileHeader header_;

  // In-memory mirror of the on-disk free list, rebuilt on open. Exists to
  // catch double frees immediately and corrupt (cyclic) lists at open.
  std::unordered_set<page_id_t> free_pages_;
};
}  // namespace tinydb
