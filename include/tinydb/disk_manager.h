#pragma once

#include <tinydb/file_header.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <filesystem>
#include <unordered_set>
#include <utility>

namespace tinydb {

// DiskManager owns the database file descriptor and provides full-page I/O.
//
// This layer treats pages as opaque bytes. It does not know whether a page
// contains a B+ tree node, free page, overflow page, or any higher-level state.
//
// I/O failures are reported as Status / Result values; nothing here throws.
class DiskManager {
 public:
  // Opens or creates the database file at path. Fails with InvalidArgument
  // if the file exists but is not a TinyDB database, Corruption if its free
  // list is damaged, and IoError if the environment misbehaves.
  static auto Open(const std::filesystem::path &path) -> Result<DiskManager>;

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
  auto AllocatePage() -> Result<page_id_t>;

  // Puts page_id on the free list for AllocatePage to reuse. The caller
  // must own the page and drop every reference to it first. Freeing a page
  // twice is a bug and aborts.
  auto FreePage(page_id_t page_id) -> Status;

  // The B+ tree root page id persisted in the file header. HEADER_PAGE_ID
  // means the database has no root yet (freshly created file).
  auto GetRootPageId() const -> page_id_t;

  // Records a new root page id in the file header and writes it out.
  auto SetRootPageId(page_id_t root_page_id) -> Status;

  // Blocks until every write so far has reached the storage device, not just
  // the OS page cache. This is the durability point for a clean close.
  auto Sync() const -> Status;

  // Reads exactly one page into data. Reading an unallocated page is an error.
  auto ReadPage(page_id_t page_id, char *data) const -> Status;

  // Writes exactly one page from data.
  auto WritePage(page_id_t page_id, const char *data) const -> Status;

 private:
  explicit DiskManager(UniqueFd fd) : fd_(std::move(fd)) {}

  // Writes the in-memory file header to page 0.
  auto WriteHeader() const -> Status;

  // An invalid fd means this object was moved from.
  UniqueFd fd_;
  FileHeader header_{};

  // In-memory mirror of the on-disk free list, rebuilt on open. Exists to
  // catch double frees immediately and corrupt (cyclic) lists at open.
  std::unordered_set<page_id_t> free_pages_;
};
}  // namespace tinydb
