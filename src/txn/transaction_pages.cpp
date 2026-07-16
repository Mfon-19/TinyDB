#include "txn/transaction_pages.h"

#include <tinydb/check.h>

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace tinydb::txn {

namespace {

// The provisional value sorts after every real LSN, so coalescing a newly
// retired page with an older extent cannot accidentally make it reusable.
constexpr auto PENDING_COMMIT_LSN = std::numeric_limits<std::uint64_t>::max();

}  // namespace

/*
** Open a write overlay on one immutable base state. The allocator index is
** decoded eagerly so corruption is reported before callers begin modifying
** tree pages. A successful return owns no page lease.
*/
auto TransactionPages::Begin(PageReader *committed, DatabaseState base_state,
                             std::size_t memory_limit_bytes) -> Result<TransactionPages> {
  if (committed == nullptr) {
    return std::unexpected(Status::InvalidArgument("transaction requires committed pages"));
  }
  if (memory_limit_bytes < PAGE_SIZE) {
    return std::unexpected(Status::InvalidArgument("transaction memory limit must hold at least one page"));
  }
  if (base_state.high_water_page_id < FIRST_DATA_PAGE_ID ||
      (base_state.root_page_id != HEADER_PAGE_ID && base_state.root_page_id >= base_state.high_water_page_id) ||
      (base_state.allocator_root_page_id != HEADER_PAGE_ID &&
       base_state.allocator_root_page_id >= base_state.high_water_page_id)) {
    return std::unexpected(Status::Corruption("invalid transaction base state"));
  }

  // Decode the captured allocator before returning a write context. Corrupt
  // metadata can never yield a partially usable transaction.
  auto transaction = TransactionPages(committed, base_state, memory_limit_bytes);
  if (auto status = transaction.LoadFreeExtents(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  return transaction;
}

TransactionPages::~TransactionPages() { RequireUnpinned(); }

void TransactionPages::RequireActive() const {
  TINYDB_CHECK(!frozen_ && !aborted_, "mutating an inactive page transaction");
}

void TransactionPages::RequireUnpinned() const {
  for (const auto &[page_id, page] : pages_) {
    (void)page_id;
    TINYDB_CHECK(page->pin_count == 0, "destroying or freezing a transaction with leased pages");
  }
}

void TransactionPages::ReleasePrivate(void *owner, page_id_t page_id, bool dirty) {
  auto *const frame = static_cast<PrivateFrame *>(owner);
  TINYDB_CHECK(frame->page_id == page_id, "transaction page lease changed identity");
  TINYDB_CHECK(frame->pin_count != 0, "transaction page pin count underflow");
  // Dirty is sticky across leases to this private frame; releasing it never
  // updates shared cache metadata.
  --frame->pin_count;
  frame->dirty = frame->dirty || dirty;
}

auto TransactionPages::PrivateHandle(PrivateFrame *frame, bool editable) -> PageHandle {
  ++frame->pin_count;
  return PageHandle(frame, frame->page_id, frame->bytes->data(), editable, ReleasePrivate);
}

auto TransactionPages::ChargePage() -> Status {
  // Charge first so failure leaves no page-map entry or logical reservation.
  if (memory_used_bytes_ > memory_limit_bytes_ - PAGE_SIZE) {
    return Status::ResourceExhausted("write transaction memory limit exceeded");
  }
  memory_used_bytes_ += PAGE_SIZE;
  return {};
}

auto TransactionPages::CreatePrivatePage(page_id_t page_id, bool dirty) -> Result<PrivateFrame *> {
  if (auto existing = pages_.find(page_id); existing != pages_.end()) {
    return existing->second.get();
  }
  if (auto status = ChargePage(); !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  auto frame = std::make_unique<PrivateFrame>(PrivateFrame{
      .page_id = page_id,
      .bytes = std::make_unique<cache::PageBytes>(),
      .pin_count = 0,
      .dirty = dirty,
  });
  auto *const result = frame.get();
  const auto [position, inserted] = pages_.emplace(page_id, std::move(frame));
  TINYDB_CHECK(inserted, "transaction inserted one page twice");
  (void)position;
  return result;
}

auto TransactionPages::Read(page_id_t page_id) -> Result<PageHandle> {
  if (aborted_) {
    return std::unexpected(Status::Closed("reading an aborted write transaction"));
  }
  if (retired_page_ids_.contains(page_id)) {
    return std::unexpected(Status::Corruption("transaction read a page it already retired"));
  }
  // Read-your-writes is a map lookup; committed state is consulted only when
  // this transaction has never copied or allocated the page.
  if (const auto page = pages_.find(page_id); page != pages_.end()) {
    return PrivateHandle(page->second.get(), false);
  }
  return committed_->Read(page_id);
}

auto TransactionPages::Edit(page_id_t page_id) -> Result<PageHandle> {
  RequireActive();
  if (retired_page_ids_.contains(page_id)) {
    return std::unexpected(Status::Corruption("transaction edited a page it already retired"));
  }
  if (const auto page = pages_.find(page_id); page != pages_.end()) {
    return PrivateHandle(page->second.get(), true);
  }

  // First edit is copy-on-write. Later edits resolve the stable private frame
  // through the map branch above.
  auto committed = committed_->Read(page_id);
  if (!committed) {
    return std::unexpected(std::move(committed).error());
  }
  auto private_page = CreatePrivatePage(page_id, false);
  if (!private_page) {
    return std::unexpected(std::move(private_page).error());
  }
  std::memcpy((*private_page)->bytes->data(), committed->Data(), PAGE_SIZE);
  return PrivateHandle(*private_page, true);
}

auto TransactionPages::AllocateReusablePage() -> std::optional<page_id_t> {
  for (auto extent = free_extents_.begin(); extent != free_extents_.end(); ++extent) {
    // Eligibility is fixed by the captured checkpoint frontier. A checkpoint
    // completing concurrently cannot change allocation choices mid-transaction.
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

auto TransactionPages::AllocateHighWaterPage() -> Result<PrivateFrame *> {
  if (resulting_state_.high_water_page_id == std::numeric_limits<page_id_t>::max()) {
    return std::unexpected(Status::ResourceExhausted("page ID space exhausted"));
  }
  // This is logical growth only. DiskManager extends the file after commit.
  const auto page_id = resulting_state_.high_water_page_id++;
  allocator_dirty_ = true;
  auto page = CreatePrivatePage(page_id, true);
  if (!page) {
    --resulting_state_.high_water_page_id;
    return std::unexpected(std::move(page).error());
  }
  return page;
}

auto TransactionPages::Allocate() -> Result<PageHandle> {
  RequireActive();
  // Reuse is preferred because it bounds file growth. Falling back to the
  // private frontier changes only resulting_state_, not the physical file.
  auto page_id = AllocateReusablePage();
  PrivateFrame *frame = nullptr;
  if (page_id.has_value()) {
    auto page = CreatePrivatePage(*page_id, true);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    frame = *page;
  } else {
    auto page = AllocateHighWaterPage();
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
  if (page_id < FIRST_DATA_PAGE_ID || page_id >= resulting_state_.high_water_page_id) {
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
    if (page->second->pin_count != 0) {
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
  TINYDB_CHECK(root_page_id >= FIRST_DATA_PAGE_ID && root_page_id < resulting_state_.high_water_page_id &&
                   !retired_page_ids_.contains(root_page_id),
               "transaction root is outside its allocation state");
  resulting_state_.root_page_id = root_page_id;
}

auto TransactionPages::ChargeValueBytes(std::size_t bytes) -> Status {
  RequireActive();
  if (bytes > memory_limit_bytes_ - memory_used_bytes_) {
    return Status::ResourceExhausted("write transaction memory limit exceeded");
  }
  memory_used_bytes_ += bytes;
  return {};
}

auto TransactionPages::LoadFreeExtents() -> Status {
  auto page_id = base_state_.allocator_root_page_id;
  auto visited = std::unordered_set<page_id_t>{};
  // Bounds, cycles, and global ordering are proven before any extent becomes
  // available to Allocate.
  while (page_id != HEADER_PAGE_ID) {
    if (page_id >= base_state_.high_water_page_id || !visited.insert(page_id).second) {
      return Status::Corruption("free-extent index contains an invalid link");
    }
    auto page = committed_->Read(page_id);
    if (!page) {
      return std::move(page).error();
    }
    const auto decoded = storage::DecodeFreeExtentPage(std::as_bytes(std::span{page->Data(), PAGE_SIZE}), page_id);
    if (!decoded) {
      return decoded.error();
    }
    for (const auto &extent : decoded->extents) {
      if (extent.first_page_id + extent.page_count > base_state_.high_water_page_id) {
        return Status::Corruption("free extent exceeds the allocation frontier");
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
  // Retain metadata IDs separately from free extents. Integrity verification
  // and allocation must treat allocator pages as allocated but non-tree pages.
  return {};
}

/*
** Add this transaction's retirements to the persistent extent model. Extents
** are sorted and coalesced so the encoded index is canonical. Coalescing uses
** the newest LSN of adjacent ranges: this may delay reuse of an older page but
** can never permit reuse too early.
*/
void TransactionPages::AddRetiredExtents(std::uint64_t retire_lsn) {
  auto retired = std::vector<page_id_t>(retired_page_ids_.begin(), retired_page_ids_.end());
  std::ranges::sort(retired);
  for (const auto page_id : retired) {
    free_extents_.push_back(storage::FreeExtent{.first_page_id = page_id, .page_count = 1, .retire_lsn = retire_lsn});
  }
  std::ranges::sort(free_extents_, {}, &storage::FreeExtent::first_page_id);

  // Adjacent extents inherit the newer retirement LSN. Delayed reuse is safe;
  // early reuse would not be.
  auto coalesced = std::vector<storage::FreeExtent>{};
  for (const auto &extent : free_extents_) {
    if (!coalesced.empty() && coalesced.back().first_page_id + coalesced.back().page_count == extent.first_page_id) {
      coalesced.back().page_count += extent.page_count;
      coalesced.back().retire_lsn = std::max(coalesced.back().retire_lsn, extent.retire_lsn);
    } else {
      coalesced.push_back(extent);
    }
  }
  free_extents_ = std::move(coalesced);
}

auto TransactionPages::StoreFreeExtentIndex(std::uint64_t page_lsn) -> Status {
  if (free_extents_.empty() && allocator_page_ids_.empty()) {
    resulting_state_.allocator_root_page_id = HEADER_PAGE_ID;
    return {};
  }
  const auto required_pages = std::max<std::size_t>(
      1, (free_extents_.size() + storage::FREE_EXTENTS_PER_PAGE - 1) / storage::FREE_EXTENTS_PER_PAGE);
  // Metadata growth bypasses the free index it is modifying and consumes only
  // high-water IDs. Existing metadata pages are retained when the index shrinks.
  while (allocator_page_ids_.size() < required_pages) {
    auto page = AllocateHighWaterPage();
    if (!page) {
      return std::move(page).error();
    }
    allocator_page_ids_.push_back((*page)->page_id);
  }

  // Rewrite the whole metadata chain. Unused retained pages encode empty
  // ranges rather than being retired recursively while the index is changing.
  for (std::size_t index = 0; index < allocator_page_ids_.size(); ++index) {
    auto page = CreatePrivatePage(allocator_page_ids_[index], true);
    if (!page) {
      return std::move(page).error();
    }
    const auto first = index * storage::FREE_EXTENTS_PER_PAGE;
    const auto count =
        first < free_extents_.size() ? std::min(storage::FREE_EXTENTS_PER_PAGE, free_extents_.size() - first) : 0;
    const auto next = index + 1 < allocator_page_ids_.size() ? allocator_page_ids_[index + 1] : HEADER_PAGE_ID;
    const auto encoded =
        storage::EncodeFreeExtentPage(allocator_page_ids_[index], page_lsn, next,
                                      std::span<const storage::FreeExtent>{free_extents_}.subspan(first, count));
    if (!encoded) {
      return encoded.error();
    }
    std::memcpy((*page)->bytes->data(), encoded->data(), PAGE_SIZE);
    (*page)->dirty = true;
  }
  resulting_state_.allocator_root_page_id = allocator_page_ids_.front();
  return {};
}

auto TransactionPages::Freeze() -> Status {
  RequireActive();
  RequireUnpinned();
  // Allocator serialization comes first because it can add metadata pages and
  // advance the resulting high-water frontier.
  if (allocator_dirty_) {
    AddRetiredExtents(PENDING_COMMIT_LSN);
    if (auto status = StoreFreeExtentIndex(PENDING_COMMIT_LSN); !status.Ok()) {
      return status;
    }
  }
  // A transaction identity advances for any logical state change, even if the
  // change consists only of a root/frontier/retirement metadata update.
  const auto changed = std::ranges::any_of(pages_, [](const auto &entry) { return entry.second->dirty; }) ||
                       resulting_state_.root_page_id != base_state_.root_page_id ||
                       resulting_state_.allocator_root_page_id != base_state_.allocator_root_page_id ||
                       resulting_state_.high_water_page_id != base_state_.high_water_page_id ||
                       !retired_page_ids_.empty();
  if (changed) {
    resulting_state_.transaction_id = base_state_.transaction_id + 1;
  }
  frozen_ = true;
  return {};
}

auto TransactionPages::Seal(std::uint64_t commit_lsn) -> Status {
  TINYDB_CHECK(frozen_ && !sealed_ && !aborted_, "sealing a transaction outside its frozen state");
  TINYDB_CHECK(commit_lsn != 0 && commit_lsn != PENDING_COMMIT_LSN, "sealing an invalid commit LSN");
  RequireUnpinned();

  const auto page_count = pages_.size();
  const auto high_water = resulting_state_.high_water_page_id;
  if (allocator_dirty_) {
    for (auto &extent : free_extents_) {
      if (extent.retire_lsn == PENDING_COMMIT_LSN) {
        extent.retire_lsn = commit_lsn;
      }
    }
    if (auto status = StoreFreeExtentIndex(commit_lsn); !status.Ok()) {
      return status;
    }
    // Freeze already allocated every allocator frame. A change here would make
    // the previously computed commit LSN circular and is an internal bug.
    TINYDB_CHECK(pages_.size() == page_count && resulting_state_.high_water_page_id == high_water,
                 "sealing changed the frozen transaction page set");
  }

  // Page LSNs and their checksums are the final bytes consumed by both WAL and
  // the prepared cache frames.
  for (auto &[page_id, page] : pages_) {
    if (!page->dirty || retired_page_ids_.contains(page_id)) {
      continue;
    }
    if (auto status = storage::RewriteDataPageLsn(std::as_writable_bytes(std::span<char, PAGE_SIZE>(*page->bytes)),
                                                  page_id, commit_lsn);
        !status.Ok()) {
      return status;
    }
  }
  resulting_state_.visible_lsn = commit_lsn;
  RequireUnpinned();
  sealed_ = true;
  return {};
}

void TransactionPages::Abort() noexcept {
  // Private ownership is the undo record. Dropping it restores the complete
  // committed state without replaying inverse operations.
  RequireUnpinned();
  pages_.clear();
  retired_page_ids_.clear();
  free_extents_.clear();
  allocator_page_ids_.clear();
  memory_used_bytes_ = 0;
  aborted_ = true;
}

auto TransactionPages::HasChanges() const -> bool {
  TINYDB_CHECK(frozen_, "checking transaction changes before freeze");
  return resulting_state_ != base_state_;
}

auto TransactionPages::FinalPageCount() const -> std::size_t {
  TINYDB_CHECK(frozen_, "counting transaction pages before freeze");
  return std::ranges::count_if(
      pages_, [this](const auto &entry) { return entry.second->dirty && !retired_page_ids_.contains(entry.first); });
}

auto TransactionPages::ResultingState() const -> const DatabaseState & {
  TINYDB_CHECK(frozen_, "reading transaction state before freeze");
  return resulting_state_;
}

auto TransactionPages::PageImages() const -> std::vector<std::pair<page_id_t, const char *>> {
  TINYDB_CHECK(sealed_, "reading transaction pages before seal");
  // Stable ordering makes WAL construction deterministic and lets a commit
  // digest bind one unambiguous physical transaction representation.
  auto result = std::vector<std::pair<page_id_t, const char *>>{};
  result.reserve(pages_.size());
  for (const auto &[page_id, page] : pages_) {
    if (page->dirty && !retired_page_ids_.contains(page_id)) {
      result.emplace_back(page_id, page->bytes->data());
    }
  }
  std::ranges::sort(result, {}, &std::pair<page_id_t, const char *>::first);
  return result;
}

auto TransactionPages::TakePages(std::uint64_t transaction_id) -> Result<std::vector<cache::CommittedPageImage>> {
  TINYDB_CHECK(sealed_, "taking transaction pages before seal");
  RequireUnpinned();
  auto result = std::vector<cache::CommittedPageImage>{};
  result.reserve(pages_.size());
  // Ownership moves directly toward publication; page bytes are not copied.
  for (auto &[page_id, page] : pages_) {
    if (!page->dirty || retired_page_ids_.contains(page_id)) {
      continue;
    }
    const auto header =
        storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*page->bytes)), page_id);
    if (!header) {
      return std::unexpected(header.error());
    }
    result.push_back(cache::CommittedPageImage{
        .page_id = page_id,
        .page_lsn = header->page_lsn,
        .transaction_id = transaction_id,
        .bytes = std::move(page->bytes),
    });
  }
  // Any clean private copy served read-your-writes but has no committed image
  // to transfer. Clearing the map releases those copies with the dirty ones.
  pages_.clear();
  memory_used_bytes_ = 0;
  return result;
}

}  // namespace tinydb::txn
