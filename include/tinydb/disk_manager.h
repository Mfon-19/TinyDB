#pragma once

#include <tinydb/file_header.h>
#include <tinydb/status.h>
#include <tinydb/unique_fd.h>

#include <array>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {

// A full page image synthesized from this layer's in-memory metadata (the
// file header, a free-list link) for the engine to write-ahead log; see
// DiskManager::TakeOpImages.
struct PageImage {
  page_id_t page_id;
  std::array<char, PAGE_SIZE> data;
};

// DiskManager owns the database file descriptor and provides full-page I/O.
//
// This layer treats pages as opaque bytes. It does not know whether a page
// contains a B+ tree node, free page, overflow page, or any higher-level state.
//
// Metadata mutations (allocating, freeing, the root page id) happen in
// memory only: mid-operation their durability is the write-ahead log's job
// (the engine logs what TakeOpImages hands out), and they reach the
// database file itself only at Checkpoint(). Growing the file is the one
// eager write — file size is physical state, not logical state; the
// header's next_page_id is what says which pages exist.
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

  // Puts page_id on the free list for AllocatePage to reuse — in memory
  // only; the link is pending until TakeOpImages logs it and a checkpoint
  // writes it. The caller must own the page and drop every reference to it
  // first. Freeing a page twice is a bug and aborts.
  void FreePage(page_id_t page_id);

  // The B+ tree root page id persisted in the file header. HEADER_PAGE_ID
  // means the database has no root yet (freshly created file).
  auto GetRootPageId() const -> page_id_t;

  // Records a new root page id in the in-memory file header.
  void SetRootPageId(page_id_t root_page_id);

  // Page images for everything this layer changed in memory during the
  // operation in flight: the file header if it changed, plus a free-list
  // link image for every page the operation freed. The engine logs these
  // alongside the operation's dirty frames; calling this drains the per-op
  // state, so call it exactly once per operation, before the WAL commit.
  auto TakeOpImages() -> std::vector<PageImage>;

  // Writes the deferred metadata — every pending free-list link and the
  // file header — to the database file. Only call between operations, and
  // only as part of a checkpoint (followed by Sync, then the log reset):
  // mid-operation the log carries these changes instead.
  auto Checkpoint() -> Status;

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

  // Free-list links not yet written to their pages: page -> next_free.
  // Cumulative since the last checkpoint, drained by Checkpoint(). A link
  // whose page gets reallocated is dropped: the page's new contents
  // supersede it, and the LIFO free list guarantees nothing still points
  // at it from below.
  std::unordered_map<page_id_t, page_id_t> pending_free_links_;

  // Pages freed by the operation in flight; TakeOpImages() drains it.
  std::vector<page_id_t> op_freed_pages_;

  // The in-memory header has diverged from the last image handed out.
  // Set by allocate/free/new-root, cleared when TakeOpImages includes a
  // header image (open-time bootstrap changes ride into the first
  // operation's images this way).
  bool header_changed_{false};
};
}  // namespace tinydb
