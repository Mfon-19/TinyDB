#include <tinydb/buffer_pool.h>
#include <tinydb/check.h>

#include "btree/page_format.h"
#include "storage/page_codec.h"

#include <cstdio>
#include <expected>
#include <utility>

/*
  BufferPool caches a fixed set of database pages in memory, and it is the
  only road between the B+ tree and the disk: the tree never does I/O, it
  borrows pointers into frames the pool has pinned for it.

  A frame's life is governed by three fields, and most of this file is the
  discipline around them:

      pin_count   how many callers hold this frame's data pointer right
                  now. A pinned frame is immovable — eviction skips it —
                  so the pointer stays valid until every pin is returned.

      dirty       the frame's bytes differ from the database file's, so
                  they must be written back before the frame is reused.

      op_dirty    dirty, and specifically dirtied by the operation
                  currently in flight: uncommitted bytes.

  op_dirty is the pool's half of the write-ahead-logging bargain (the
  engine's half lives in storage_engine.cpp). The log is redo-only — it
  can replay committed changes but holds no undo images — so an
  uncommitted change that reached the database file could never be scrubbed
  out after a crash. The pool therefore enforces no-steal:

      1. The engine brackets every mutation with BeginOp / EndOp.
      2. Every frame dirtied inside the bracket is marked op_dirty.
      3. An op_dirty frame is never evicted and never flushed: its bytes
         cannot reach the database file by any path.
      4. OpDirtyFrames hands the engine exactly those frames — the images
         it must log before the operation may commit.
      5. EndOp clears the marks once the log has made those images
         durable, demoting the frames to ordinary dirty: from that moment
         writing them to the database file is harmless, because a crash
         can rebuild them from the log.

  The failure path is the quiet fifth rule's mirror: a mutation that dies
  midway poisons the engine, which never calls EndOp, so the half-written
  frames stay quarantined until the process exits and the best-effort
  teardown flush skips them. The database file only ever sees bytes the
  log can vouch for.

  Between operations the pool is a plain write-back cache: dirty pages
  linger in memory until a checkpoint flushes them or eviction needs the
  frame. Eviction is a rotating sweep — a hand advances round-robin,
  resuming where it last stopped, and takes the first frame that is
  neither pinned nor op_dirty, writing it back first if dirty. That is
  CLOCK with no reference bit: nothing distinguishes hot frames from cold
  ones, so the policy approximates FIFO rather than LRU. Fine at this
  scale; a reference bit is future work if scan patterns ever matter.

  What the pool requires of its callers:

  - Pins must balance. Every FetchPage / NewPage is one pin; each pin is
    returned by exactly one UnpinPage, with an honest dirty flag. PageRef
    (page_ref.h) exists so this bookkeeping happens by scope, not by hand.

  - FreePage only for a page with no pins and no live pointers. Its cached
    bytes are discarded, not flushed — freeing declares them dead.

  - EndOp only after the operation's logged images are durable. Calling it
    earlier reopens the hole no-steal exists to close: eviction could then
    write uncommitted bytes to the database file.

  - Borrowed pointers (FetchPage, NewPage, OpDirtyFrames) are stable while
    the frame is pinned; the OpDirtyFrames pointers specifically are only
    good until the pool next fetches or allocates, which may recycle any
    unpinned frame.
*/

namespace tinydb {

// Every frame starts on the free list; the page table fills as pages are
// fetched and allocated.
BufferPool::BufferPool(DiskManager *disk_manager, std::size_t frame_count)
    : disk_manager_(disk_manager), frames_(frame_count) {
  for (frame_id_t frame_id = 0; frame_id < frames_.size(); ++frame_id) {
    free_list_.push_back(frame_id);
  }
}

BufferPool::BufferPool(BufferPool &&other) noexcept
    : disk_manager_(other.disk_manager_),
      frames_(std::move(other.frames_)),
      page_table_(std::move(other.page_table_)),
      free_list_(std::move(other.free_list_)),
      next_victim_(other.next_victim_),
      in_op_(other.in_op_) {
  other.disk_manager_ = nullptr;
}

auto BufferPool::operator=(BufferPool &&other) noexcept -> BufferPool & {
  if (this != &other) {
    // The pool being overwritten flushes its own dirty pages first;
    // dropping them silently would lose writes that already committed.
    FlushBestEffort();

    disk_manager_ = other.disk_manager_;
    frames_ = std::move(other.frames_);
    page_table_ = std::move(other.page_table_);
    free_list_ = std::move(other.free_list_);
    next_victim_ = other.next_victim_;
    in_op_ = other.in_op_;

    other.disk_manager_ = nullptr;
  }

  return *this;
}

BufferPool::~BufferPool() { FlushBestEffort(); }

// FlushAllPages for the destructor and move assignment, which have no way
// to surface a status: failures are printed to stderr and dropped. Callers
// who must handle flush errors — the engine's Close() — flush through the
// owning handle before destruction ever gets here.
void BufferPool::FlushBestEffort() noexcept {
  if (const auto status = FlushAllPages(); !status.Ok()) {
    std::fprintf(stderr, "tinydb: buffer pool flush failed: %s\n", status.ToString().c_str());
  }
}

auto BufferPool::NewPage() -> Result<NewPageResult> {
  /*
    Allocate a fresh page and hand back its zeroed, already-pinned frame.

    The frame is dirty from birth: AllocatePage grew the file or reused a
    freed page, and either way the on-disk bytes are unspecified, so the
    file's copy is garbage until these zeroes — and whatever the caller
    writes over them — get flushed. Inside an operation the frame is
    op_dirty from birth too: a new page's first contents are uncommitted
    like any other mutation, so its image is always among the ones logged.
    (The superblock image that records the bumped next_page_id therefore never
    travels without the new page's image beside it — recovery's page-count
    check in disk_manager.cpp leans on that.)
  */
  const auto frame_id = PickFrame();
  if (!frame_id) {
    return std::unexpected(frame_id.error());
  }
  const auto new_page_id = disk_manager_->AllocatePage();
  if (!new_page_id) {
    return std::unexpected(new_page_id.error());
  }
  auto &frame = frames_[*frame_id];

  frame.page_id = *new_page_id;
  frame.data.fill(0);
  frame.pin_count = 1;
  frame.dirty = true;
  frame.op_dirty = in_op_;
  page_table_[*new_page_id] = *frame_id;

  return NewPageResult{.page_id = *new_page_id, .data = frame.data.data()};
}

auto BufferPool::FetchPage(page_id_t page_id) -> Result<char *> {
  /*
    Pin page_id and return a pointer to its bytes. A cache hit is just one
    more pin on the frame already holding the page — any number of callers
    may borrow the same frame, and the page table guarantees a page never
    occupies two frames. A miss claims a frame (evicting someone if none
    are free), reads the page into it, and pins it for the first time.
  */
  const auto page_it = page_table_.find(page_id);
  if (page_it != page_table_.end()) {
    auto &frame = frames_[page_it->second];
    ++frame.pin_count;
    return frame.data.data();
  }

  const auto frame_id = PickFrame();
  if (!frame_id) {
    return std::unexpected(frame_id.error());
  }
  auto &frame = frames_[*frame_id];

  if (auto status = disk_manager_->ReadPage(page_id, frame.data.data()); !status.Ok()) {
    // Return the frame to the free list under the sentinel id, exactly as
    // FreePage does. Abandoning it with its old occupant's id would be a
    // time bomb: once that page is cached again elsewhere, an eviction
    // sweep landing on this frame would erase the page's *live* mapping,
    // and the pool would cache the same page in two frames at once.
    frame.page_id = HEADER_PAGE_ID;
    free_list_.push_back(*frame_id);
    return std::unexpected(std::move(status));
  }
  const auto common = storage::DecodeDataPageHeader(std::as_bytes(std::span{frame.data}), page_id);
  auto validation = common ? Status{} : common.error();
  if (common && (common->type == storage::DataPageType::Leaf || common->type == storage::DataPageType::Internal)) {
    validation = ValidateTreePage(frame.data.data(), page_id);
  }
  if (!validation.Ok()) {
    frame.page_id = HEADER_PAGE_ID;
    free_list_.push_back(*frame_id);
    return std::unexpected(std::move(validation));
  }
  frame.page_id = page_id;
  frame.pin_count = 1;
  frame.dirty = false;
  frame.op_dirty = false;
  page_table_[page_id] = *frame_id;

  return frame.data.data();
}

// Returns one pin, reporting honestly whether the caller wrote to the
// frame. Both flags are sticky ORs: one writer among many readers keeps
// the frame dirty until its bytes reach the file, and op_dirty latches
// only inside a BeginOp/EndOp bracket — open-time bootstrap writes happen
// outside any bracket and stay plain dirty, because they are not part of
// a logged operation (a reopened database recreates them from scratch
// instead of replaying them).
void BufferPool::UnpinPage(page_id_t page_id, bool dirty) {
  const auto page_it = page_table_.find(page_id);
  TINYDB_CHECK(page_it != page_table_.end(), "unpinning a page that is not in the pool");

  auto &frame = frames_[page_it->second];
  TINYDB_CHECK(frame.pin_count > 0, "unpinning an unpinned page");

  --frame.pin_count;
  frame.dirty = frame.dirty || dirty;
  frame.op_dirty = frame.op_dirty || (dirty && in_op_);
}

// Opens the mutation bracket: from here until EndOp, every frame the
// caller dirties is quarantined as op_dirty (see the file comment).
// Single-operation engine, so brackets never nest.
void BufferPool::BeginOp() {
  TINYDB_CHECK(!in_op_, "beginning an operation before the previous one ended");
  in_op_ = true;
}

// The images the engine must log before this operation may commit: every
// frame the bracket dirtied, as (page id, bytes) pairs. The pointers are
// borrowed straight from the frames — good until the pool next fetches or
// allocates — and the frames stay quarantined; only EndOp releases them.
auto BufferPool::OpDirtyFrames() const -> std::vector<std::pair<page_id_t, const char *>> {
  TINYDB_CHECK(in_op_, "collecting op-dirty frames outside an operation");

  std::vector<std::pair<page_id_t, const char *>> images;
  for (const auto &frame : frames_) {
    if (frame.op_dirty) {
      images.emplace_back(frame.page_id, frame.data.data());
    }
  }
  return images;
}

// Closes the bracket, demoting every op_dirty frame to ordinary dirty —
// evictable and flushable again. Correct only once the logged images are
// durable (Wal::Commit returned Ok), or when the bracket dirtied nothing.
// The failure path never gets here on purpose: a poisoned engine leaves
// the bracket open, so its half-written frames stay quarantined through
// teardown and the best-effort flush skips them.
void BufferPool::EndOp() {
  TINYDB_CHECK(in_op_, "ending an operation that never began");

  for (auto &frame : frames_) {
    frame.op_dirty = false;
  }
  in_op_ = false;
}

void BufferPool::FreePage(page_id_t page_id) {
  /*
    The tree just unlinked this page: drop any cached copy, then hand the
    page to the disk manager's free list for reuse.

    The cached bytes are discarded, never flushed — freeing declares them
    dead, and both dirty flags are cleared so no write-back path ever
    resurrects them. That holds even mid-operation: an op-dirty page freed
    by the same operation needs nothing logged for its content, because
    the free-list link that replaces it travels separately through
    DiskManager::TakeOpImages.

    The page may legitimately not be cached at all (its frame was evicted
    earlier); the disk-side free still happens.
  */
  const auto page_it = page_table_.find(page_id);
  if (page_it != page_table_.end()) {
    auto &frame = frames_[page_it->second];
    TINYDB_CHECK(frame.pin_count == 0, "freeing a pinned page");

    frame.page_id = HEADER_PAGE_ID;
    frame.dirty = false;
    frame.op_dirty = false;
    free_list_.push_back(page_it->second);
    page_table_.erase(page_it);
  }

  disk_manager_->FreePage(page_id);
}

// Writes one page's dirty bytes to the database file, if it is cached and
// dirty at all — a clean or absent page is a successful no-op, since the
// file already has its latest bytes. An op_dirty frame is left alone: its
// bytes are uncommitted, and no-steal says they never reach the file.
auto BufferPool::FlushPage(page_id_t page_id) -> Status {
  const auto page_it = page_table_.find(page_id);
  if (page_it == page_table_.end()) {
    return {};
  }

  auto &frame = frames_[page_it->second];
  if (frame.dirty && !frame.op_dirty) {
    if (auto status = disk_manager_->WritePage(frame.page_id, frame.data.data()); !status.Ok()) {
      return status;
    }
    frame.dirty = false;
  }
  return {};
}

auto BufferPool::FlushAllPages() -> Status {
  /*
    The checkpoint's bulk write: every committed-dirty frame goes to the
    database file. Frames are marked clean one by one as they land, so a
    failed flush can be retried and only rewrites the remainder.

    Op-dirty frames are skipped, not flushed. During normal operation
    there are none (checkpoints run between operations); the case that
    matters is teardown of a poisoned engine, whose abandoned mutation is
    still quarantined — those bytes must never reach the database file,
    which belongs to the log's committed images alone.
  */
  if (disk_manager_ == nullptr) {
    return {};
  }

  for (auto &frame : frames_) {
    if (frame.dirty && !frame.op_dirty) {
      if (auto status = disk_manager_->WritePage(frame.page_id, frame.data.data()); !status.Ok()) {
        return status;
      }
      frame.dirty = false;
    }
  }
  return {};
}

auto BufferPool::PickFrame() -> Result<frame_id_t> {
  /*
    Claim a frame for a new occupant: off the free list if one is spare,
    otherwise evict somebody. The eviction hand sweeps round-robin from
    wherever it stopped last time, and takes the first frame that is

        unpinned     — nobody is holding its pointer, and
        not op_dirty — its bytes are not uncommitted work,

    writing the frame back first if it is dirty. That write-back is safe
    precisely because of what op_dirty rules out: a dirty-but-not-op-dirty
    frame holds either a committed change, whose image is already durable
    in the log, or an open-time bootstrap write, which a reopen would
    recreate — nothing that could corrupt the file if a crash follows.

    A full lap with no taker means every frame is pinned or quarantined by
    the in-flight operation: the pool is too small for the working set of
    a single operation. That is ResourceExhausted, and it fails the
    operation before any half-measure (evicting a pinned page would
    invalidate a pointer some caller still holds).
  */
  if (!free_list_.empty()) {
    const auto frame_id = free_list_.back();
    free_list_.pop_back();
    return frame_id;
  }

  for (std::size_t count = 0; count < frames_.size(); ++count) {
    const auto frame_id = next_victim_;
    next_victim_ = (next_victim_ + 1) % frames_.size();
    auto &frame = frames_[frame_id];

    if (frame.pin_count == 0 && !frame.op_dirty) {
      if (frame.dirty) {
        if (auto status = disk_manager_->WritePage(frame.page_id, frame.data.data()); !status.Ok()) {
          return std::unexpected(std::move(status));
        }
      }
      page_table_.erase(frame.page_id);
      frame.dirty = false;
      return frame_id;
    }
  }

  return std::unexpected(Status::ResourceExhausted("no evictable frame"));
}

}  // namespace tinydb
