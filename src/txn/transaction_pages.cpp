#include "txn/transaction_pages.h"

#include "util/check.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace tinydb::txn {

/*
** Open a write overlay on one immutable base state. The allocator index is
** decoded eagerly so corruption is reported before callers begin modifying
** tree pages. A successful return owns no page lease.
*/
auto TransactionPages::Begin(PageReader *committed, DatabaseState base_state, std::size_t memory_limit_bytes,
                             std::shared_ptr<cache::PageArena> page_arena) -> Result<TransactionPages> {
  if (base_state.logical_page_count < FIRST_DATA_PAGE_ID ||
      (base_state.root_page_id != HEADER_PAGE_ID && base_state.root_page_id >= base_state.logical_page_count) ||
      (base_state.allocator_root_page_id != HEADER_PAGE_ID &&
       base_state.allocator_root_page_id >= base_state.logical_page_count)) {
    return std::unexpected(Status::Corruption("invalid transaction base state"));
  }

  try {
    if (page_arena == nullptr) {
      page_arena = cache::PageArena::CreateHeap();
    }
  } catch (const std::bad_alloc &) {
    return std::unexpected(Status::ResourceExhausted("transaction page arena allocation failed"));
  }
  auto transaction = TransactionPages(committed, base_state, memory_limit_bytes, std::move(page_arena));
  if (auto status = transaction.LoadFreeExtents(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return transaction;
}

TransactionPages::~TransactionPages() {
  RequireUnpinned();
  pages_.clear();
  if (page_arena_ != nullptr) {
    page_arena_->ReleaseFreeMemory();
  }
}

void TransactionPages::RequireActive() const { TINYDB_CHECK(!prepared_, "mutating a prepared page transaction"); }

void TransactionPages::RequireUnpinned() const {
  for (const auto &[page_id, page] : pages_) {
    (void)page_id;
    TINYDB_CHECK(page.pin_count == 0, "destroying or freezing a transaction with leased pages");
  }
}

void TransactionPages::ReleasePrivate(void *owner, page_id_t page_id, bool dirty, PagePayloadProof payload_proof) {
  auto *const frame = static_cast<PrivateFrame *>(owner);
  (void)page_id;
  TINYDB_CHECK(frame->pin_count != 0, "transaction page pin count underflow");
  if (frame->editing) {
    TINYDB_CHECK(frame->pin_count == 1, "mutable transaction page lease overlapped another lease");
    frame->editing = false;
  }
  // Dirty is sticky. The structural proof describes the current private bytes
  // and is replaced only by the sole mutable lease.
  --frame->pin_count;
  frame->dirty = frame->dirty || dirty;
  frame->payload_proof = payload_proof;
}

auto TransactionPages::PrivateHandle(PrivateFrame *frame, bool editable) -> PageHandle {
  if (editable) {
    TINYDB_CHECK(frame->pin_count == 0, "editing a transaction page with an outstanding lease");
    frame->editing = true;
  } else {
    TINYDB_CHECK(!frame->editing, "reading a transaction page through an active mutable lease");
  }
  ++frame->pin_count;
  return {frame, frame->page_id, frame->bytes->data(), editable, ReleasePrivate, frame->payload_proof};
}

auto TransactionPages::Charge(std::size_t bytes) -> Status {
  if (bytes > memory_limit_bytes_ - memory_used_bytes_) {
    return Status::ResourceExhausted("write transaction memory limit exceeded");
  }
  memory_used_bytes_ += bytes;
  return {};
}

auto TransactionPages::CreatePrivatePage(page_id_t page_id, bool dirty) -> Result<PrivateFrame *> {
  if (auto existing = pages_.find(page_id); existing != pages_.end()) {
    return &existing->second;
  }
  // Charge first so failure leaves no page-map entry or logical reservation.
  if (auto status = Charge(PAGE_SIZE); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto bytes = page_arena_->Acquire();
  if (!bytes) {
    memory_used_bytes_ -= PAGE_SIZE;
    return std::unexpected(Status::ResourceExhausted("transaction page allocation failed"));
  }
  auto frame = PrivateFrame{
      .page_id = page_id,
      .bytes = std::move(bytes),
      .pin_count = 0,
      .editing = false,
      .dirty = dirty,
      .payload_proof = PagePayloadProof::None,
      .sealed_header = {},
  };
  const auto position = pages_.emplace(page_id, std::move(frame)).first;
  return &position->second;
}

auto TransactionPages::Read(page_id_t page_id) -> Result<PageHandle> {
  if (retired_page_ids_.contains(page_id)) {
    return std::unexpected(Status::Corruption("transaction read a page it already retired"));
  }
  if (const auto page = pages_.find(page_id); page != pages_.end()) {
    return PrivateHandle(&page->second, false);
  }
  return committed_->Read(page_id);
}

auto TransactionPages::Edit(page_id_t page_id) -> Result<PageHandle> {
  RequireActive();
  if (retired_page_ids_.contains(page_id)) {
    return std::unexpected(Status::Corruption("transaction edited a page it already retired"));
  }
  if (const auto page = pages_.find(page_id); page != pages_.end()) {
    return PrivateHandle(&page->second, true);
  }

  auto committed = committed_->Read(page_id);
  if (!committed) {
    return std::unexpected(std::move(committed).error());
  }
  auto private_page = CreatePrivatePage(page_id, false);
  if (!private_page) {
    return std::unexpected(std::move(private_page).error());
  }
  std::memcpy((*private_page)->bytes->data(), committed->Data(), PAGE_SIZE);
  (*private_page)->payload_proof = committed->PayloadProof();
  return PrivateHandle(*private_page, true);
}

auto TransactionPages::AllocateReusablePage() -> std::optional<page_id_t> {
  for (auto extent = free_extents_.begin(); extent != free_extents_.end(); ++extent) {
    // Eligibility is fixed by the captured checkpoint LSN. A concurrent
    // checkpoint cannot change allocation choices during this transaction.
    if (extent->retire_lsn > base_state_.checkpoint_lsn) {
      continue;
    }
    const auto page_id = extent->first_page_id;
    ++extent->first_page_id;
    --extent->page_count;
    if (extent->page_count == 0) {
      free_extents_.erase(extent);
    }
    allocator_dirty_ = true;
    return page_id;
  }
  return std::nullopt;
}

auto TransactionPages::AllocateNewPage() -> Result<PrivateFrame *> {
  if (resulting_state_.logical_page_count == std::numeric_limits<page_id_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("page ID space exhausted"));
  }
  // This extends only the transaction's logical page range. The logical page
  // count is part of DatabaseState, not the free-extent index. DiskManager
  // extends the physical file after commit.
  const auto page_id = resulting_state_.logical_page_count++;
  auto page = CreatePrivatePage(page_id, true);
  if (!page) {
    --resulting_state_.logical_page_count;
    return std::unexpected(std::move(page).error());
  }
  return page;
}

auto TransactionPages::Allocate() -> Result<PageHandle> {
  RequireActive();
  auto page_id = AllocateReusablePage();
  PrivateFrame *frame = nullptr;
  if (page_id.has_value()) {
    auto page = CreatePrivatePage(*page_id, true);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    frame = *page;
  } else {
    auto page = AllocateNewPage();
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    frame = *page;
  }
  frame->bytes->fill(0);
  return PrivateHandle(frame, true);
}

auto TransactionPages::Free(page_id_t page_id) -> Status {
  RequireActive();
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= resulting_state_.logical_page_count) {
    return Status::Corruption("transaction retired an unallocated page ID");
  }
  if (retired_page_ids_.contains(page_id)) {
    return Status::Corruption("transaction retired one page twice");
  }
  if (std::ranges::find(allocator_page_ids_, page_id) != allocator_page_ids_.end()) {
    return Status::Corruption("transaction retired allocator metadata");
  }
  // A retired private image must not appear in the final physical image set.
  // Removing it also refunds its page charge; the page ID itself remains
  // quarantined in retired_page_ids_ through the end of this transaction.
  if (const auto page = pages_.find(page_id); page != pages_.end()) {
    if (page->second.pin_count != 0) {
      return Status::Corruption("transaction retired a leased page");
    }
    pages_.erase(page);
    memory_used_bytes_ -= PAGE_SIZE;
  }
  retired_page_ids_.insert(page_id);
  allocator_dirty_ = true;
  return {};
}

void TransactionPages::SetRootPageId(page_id_t root_page_id) {
  RequireActive();
  resulting_state_.root_page_id = root_page_id;
}

auto TransactionPages::ChargeValueBytes(std::size_t bytes) -> Status {
  RequireActive();
  return Charge(bytes);
}

auto TransactionPages::LoadFreeExtents() -> Status {
  auto page_id = base_state_.allocator_root_page_id;
  auto visited = std::unordered_set<page_id_t>{};
  // Bounds, cycles, and global extent ordering are proven before any extent
  // becomes available to Allocate.
  while (page_id != HEADER_PAGE_ID) {
    if (page_id >= base_state_.logical_page_count || !visited.insert(page_id).second) {
      return Status::Corruption("free-extent index contains an invalid link");
    }
    auto page = committed_->Read(page_id);
    if (!page) {
      return std::move(page).error();
    }
    const auto bytes = std::as_bytes(std::span{page->Data(), PAGE_SIZE});
    const auto *const validated_header = page->ValidatedHeader();
    const auto decoded = validated_header != nullptr ? storage::DecodeFreeExtentPage(bytes, page_id, *validated_header)
                                                     : storage::DecodeFreeExtentPage(bytes, page_id);
    if (!decoded) {
      return decoded.error();
    }
    for (const auto &extent : decoded->extents) {
      if (extent.first_page_id + extent.page_count > base_state_.logical_page_count) {
        return Status::Corruption("free extent exceeds the logical page range");
      }
      if (!free_extents_.empty()) {
        const auto &previous = free_extents_.back();
        if (previous.first_page_id + previous.page_count >= extent.first_page_id) {
          return Status::Corruption("free-extent index is not globally ordered");
        }
      }
      free_extents_.push_back(extent);
    }
    allocator_page_ids_.push_back(page_id);
    page_id = decoded->next_page_id;
  }
  return {};
}

/*
** Add this transaction's retirements to the persistent extent model. Extents
** are sorted and coalesced so the encoded index is canonical. Coalescing uses
** the newest LSN of adjacent ranges: this may delay reuse of an older page but
** can never permit reuse too early.
*/
void TransactionPages::AddRetiredExtents(std::uint64_t retire_lsn) {
  free_extents_.reserve(free_extents_.size() + retired_page_ids_.size());
  for (const auto page_id : retired_page_ids_) {
    free_extents_.push_back(storage::FreeExtent{.first_page_id = page_id, .page_count = 1, .retire_lsn = retire_lsn});
  }
  std::ranges::sort(free_extents_, {}, &storage::FreeExtent::first_page_id);

  auto output = std::size_t{0};
  for (const auto extent : free_extents_) {
    if (output != 0 &&
        free_extents_[output - 1].first_page_id + free_extents_[output - 1].page_count == extent.first_page_id) {
      free_extents_[output - 1].page_count += extent.page_count;
      free_extents_[output - 1].retire_lsn = std::max(free_extents_[output - 1].retire_lsn, extent.retire_lsn);
    } else {
      free_extents_[output++] = extent;
    }
  }
  free_extents_.resize(output);
}

auto TransactionPages::EnsureFreeExtentIndexFrames() -> Status {
  if (free_extents_.empty() && allocator_page_ids_.empty()) {
    resulting_state_.allocator_root_page_id = HEADER_PAGE_ID;
    return {};
  }
  const auto required_pages = std::max<std::size_t>(
      1, (free_extents_.size() + storage::FREE_EXTENTS_PER_PAGE - 1) / storage::FREE_EXTENTS_PER_PAGE);
  // Metadata growth bypasses the free index that it modifies. It allocates new
  // page IDs instead. Existing metadata pages remain when the index shrinks.
  while (allocator_page_ids_.size() < required_pages) {
    auto page = AllocateNewPage();
    if (!page) {
      return std::move(page).error();
    }
    allocator_page_ids_.push_back((*page)->page_id);
  }

  resulting_state_.allocator_root_page_id = allocator_page_ids_.front();
  return {};
}

auto TransactionPages::StoreFreeExtentIndex() -> Status {
  const auto slice = [](const std::vector<storage::FreeExtent> &extents, std::size_t index) {
    const auto first = index * storage::FREE_EXTENTS_PER_PAGE;
    if (first >= extents.size()) {
      return std::span<const storage::FreeExtent>{};
    }
    return std::span<const storage::FreeExtent>{extents}.subspan(
        first, std::min(storage::FREE_EXTENTS_PER_PAGE, extents.size() - first));
  };
  for (std::size_t index = 0; index < allocator_page_ids_.size(); ++index) {
    const auto page_id = allocator_page_ids_[index];
    const auto next = index + 1 < allocator_page_ids_.size() ? allocator_page_ids_[index + 1] : HEADER_PAGE_ID;
    const auto current_extents = slice(free_extents_, index);
    auto page = CreatePrivatePage(page_id, true);
    if (!page) {
      return std::move(page).error();
    }
    auto bytes = std::as_writable_bytes(std::span<char, PAGE_SIZE>{(*page)->bytes->data(), PAGE_SIZE});
    if (auto status = storage::InitializeFreeExtentPage(bytes, page_id, 0, next, current_extents); !status.Ok()) {
      return status;
    }
    (*page)->dirty = true;
  }
  return {};
}

auto TransactionPages::PrepareCommit(std::uint64_t commit_lsn) -> Status {
  RequireActive();
  RequireUnpinned();
  if (allocator_dirty_) {
    AddRetiredExtents(commit_lsn);
    if (auto status = EnsureFreeExtentIndexFrames(); !status.Ok()) {
      return status;
    }
    if (auto status = StoreFreeExtentIndex(); !status.Ok()) {
      return status;
    }
  }

  for (auto &[page_id, page] : pages_) {
    if (!page.dirty) {
      continue;
    }
    auto header = storage::RewriteDataPageLsn(std::as_writable_bytes(std::span<char, PAGE_SIZE>(*page.bytes)), page_id,
                                              commit_lsn);
    if (!header) {
      return std::move(header).error();
    }
    page.sealed_header = *header;
  }
  resulting_state_.visible_lsn = commit_lsn;
  prepared_ = true;
  return {};
}

auto TransactionPages::HasChanges() const -> bool {
  return std::ranges::any_of(pages_, [](const auto &entry) { return entry.second.dirty; }) ||
         resulting_state_ != base_state_ || !retired_page_ids_.empty();
}

auto TransactionPages::ResultingState() const -> const DatabaseState & { return resulting_state_; }

auto TransactionPages::TakePages() -> std::vector<cache::CommittedPageImage> {
  TINYDB_CHECK(prepared_, "taking transaction pages before preparation");
  RequireUnpinned();
  auto result = std::vector<cache::CommittedPageImage>{};
  result.reserve(pages_.size());
  for (auto &[page_id, page] : pages_) {
    (void)page_id;
    if (!page.dirty) {
      continue;
    }
    result.push_back(cache::CommittedPageImage{
        .header = page.sealed_header,
        .bytes = std::move(page.bytes),
        .tree_payload_validated = page.payload_proof == PagePayloadProof::Tree,
    });
  }
  std::ranges::sort(result, {}, [](const cache::CommittedPageImage &image) { return image.header.page_id; });
  pages_.clear();
  page_arena_->ReleaseFreeMemory();
  memory_used_bytes_ = 0;
  return result;
}

}  // namespace tinydb::txn
