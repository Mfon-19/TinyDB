#include "txn/transaction_pages.h"

#include "util/check.h"

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
  TINYDB_CHECK(!frozen_, "mutating a frozen page transaction");
  TINYDB_CHECK(!aborted_, "mutating an aborted page transaction");
}

void TransactionPages::RequireUnpinned() const {
  for (const auto &[page_id, page] : pages_) {
    (void)page_id;
    TINYDB_CHECK(page->pin_count == 0, "destroying or freezing a transaction with leased pages");
  }
}

void TransactionPages::ReleasePrivate(void *owner, page_id_t page_id, bool dirty, bool tree_payload_validated) {
  auto *const frame = static_cast<PrivateFrame *>(owner);
  TINYDB_CHECK(frame->page_id == page_id, "transaction page lease changed identity");
  TINYDB_CHECK(frame->pin_count != 0, "transaction page pin count underflow");
  if (frame->editing) {
    TINYDB_CHECK(frame->pin_count == 1, "mutable transaction page lease overlapped another lease");
    frame->editing = false;
  }
  // Dirty is sticky. The structural proof describes the current private bytes
  // and is replaced only by the sole mutable lease.
  --frame->pin_count;
  frame->dirty = frame->dirty || dirty;
  frame->tree_payload_validated = tree_payload_validated;
}

auto TransactionPages::PrivateHandle(PrivateFrame *frame, bool editable) -> PageHandle {
  if (editable) {
    TINYDB_CHECK(frame->pin_count == 0, "editing a transaction page with an outstanding lease");
    TINYDB_CHECK(!frame->editing, "editing a transaction page through an existing mutable lease");
    frame->editing = true;
  } else {
    TINYDB_CHECK(!frame->editing, "reading a transaction page through an active mutable lease");
  }
  ++frame->pin_count;
  return {frame, frame->page_id, frame->bytes->data(), editable, ReleasePrivate, frame->tree_payload_validated};
}

auto TransactionPages::ChargePage() -> Status {
  // Charge first so failure leaves no page-map entry or logical reservation.
  if (memory_used_bytes_ > memory_limit_bytes_ - PAGE_SIZE) {
    return Status::ResourceExhausted("write transaction memory limit exceeded");
  }
  memory_used_bytes_ += PAGE_SIZE;
  return {};
}

// The analyzer does not model unique_ptr ownership after it moves into the
// unordered map; ASan and the map's RAII ownership cover this path.
// NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
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
      .editing = false,
      .dirty = dirty,
      .tree_payload_validated = false,
      .sealed_header = {},
  });
  const auto [position, inserted] = pages_.emplace(page_id, std::move(frame));
  TINYDB_CHECK(inserted, "transaction inserted one page twice");
  return position->second.get();
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
  (*private_page)->tree_payload_validated = committed->TreePayloadValidated();
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
  // This is logical growth only. The high-water mark lives in DatabaseState,
  // so extending it does not require rewriting an unchanged free-extent index.
  // DiskManager extends the physical file after commit.
  const auto page_id = resulting_state_.high_water_page_id++;
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
  TINYDB_CHECK(root_page_id >= FIRST_DATA_PAGE_ID, "transaction root is a reserved page");
  TINYDB_CHECK(root_page_id < resulting_state_.high_water_page_id,
               "transaction root lies beyond the allocation frontier");
  TINYDB_CHECK(!retired_page_ids_.contains(root_page_id), "transaction root has been retired");
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
  // Bounds, cycles, and global extent ordering are proven before any extent
  // becomes available to Allocate.
  while (page_id != HEADER_PAGE_ID) {
    if (page_id >= base_state_.high_water_page_id || !visited.insert(page_id).second) {
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
  free_extents_.reserve(free_extents_.size() + retired_page_ids_.size());
  for (const auto page_id : retired_page_ids_) {
    free_extents_.push_back(storage::FreeExtent{.first_page_id = page_id, .page_count = 1, .retire_lsn = retire_lsn});
  }
  std::ranges::sort(free_extents_, {}, &storage::FreeExtent::first_page_id);

  // Adjacent extents inherit the newer retirement LSN. Delayed reuse is safe;
  // early reuse would not be. Compact in place because the input is no longer
  // needed after sorting.
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
  // Metadata growth bypasses the free index it is modifying and consumes only
  // high-water IDs. Existing metadata pages are retained when the index shrinks.
  while (allocator_page_ids_.size() < required_pages) {
    auto page = AllocateHighWaterPage();
    if (!page) {
      return std::move(page).error();
    }
    allocator_page_ids_.push_back((*page)->page_id);
  }

  // Freeze must include every final allocator image in its page count, but the
  // exact retirement LSN is not known yet. Reserve the frames now and encode
  // them once during Seal.
  for (const auto page_id : allocator_page_ids_) {
    auto page = CreatePrivatePage(page_id, true);
    if (!page) {
      return std::move(page).error();
    }
    (*page)->dirty = true;
  }
  resulting_state_.allocator_root_page_id = allocator_page_ids_.front();
  return {};
}

auto TransactionPages::StoreFreeExtentIndex() -> Status {
  // Rewrite the whole metadata chain. Unused retained pages encode empty
  // ranges rather than being retired recursively while the index is changing.
  for (std::size_t index = 0; index < allocator_page_ids_.size(); ++index) {
    const auto page_id = allocator_page_ids_[index];
    const auto page = pages_.find(page_id);
    TINYDB_CHECK(page != pages_.end(), "allocator frame was not reserved before sealing");
    const auto first = index * storage::FREE_EXTENTS_PER_PAGE;
    const auto count =
        first < free_extents_.size() ? std::min(storage::FREE_EXTENTS_PER_PAGE, free_extents_.size() - first) : 0;
    const auto next = index + 1 < allocator_page_ids_.size() ? allocator_page_ids_[index + 1] : HEADER_PAGE_ID;
    auto bytes = std::as_writable_bytes(std::span<char, PAGE_SIZE>{page->second->bytes->data(), PAGE_SIZE});
    if (auto status = storage::InitializeFreeExtentPage(
            bytes, page_id, 0, next, std::span<const storage::FreeExtent>{free_extents_}.subspan(first, count));
        !status.Ok()) {
      return status;
    }
    page->second->dirty = true;
  }
  return {};
}

auto TransactionPages::Freeze() -> Status {
  RequireActive();
  RequireUnpinned();
  // Allocator serialization comes first because it can add metadata pages and
  // advance the resulting high-water frontier.
  if (allocator_dirty_) {
    AddRetiredExtents(PENDING_COMMIT_LSN);
    if (auto status = EnsureFreeExtentIndexFrames(); !status.Ok()) {
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
  TINYDB_CHECK(frozen_, "sealing a transaction before freeze");
  TINYDB_CHECK(!sealed_, "sealing a transaction twice");
  TINYDB_CHECK(!aborted_, "sealing an aborted transaction");
  TINYDB_CHECK(commit_lsn != 0, "sealing a zero commit LSN");
  TINYDB_CHECK(commit_lsn != PENDING_COMMIT_LSN, "sealing the provisional commit LSN");
  RequireUnpinned();

  const auto page_count = pages_.size();
  const auto high_water = resulting_state_.high_water_page_id;
  if (allocator_dirty_) {
    for (auto &extent : free_extents_) {
      if (extent.retire_lsn == PENDING_COMMIT_LSN) {
        extent.retire_lsn = commit_lsn;
      }
    }
    if (auto status = StoreFreeExtentIndex(); !status.Ok()) {
      return status;
    }
    // Freeze already allocated every allocator frame. A change here would make
    // the previously computed commit LSN circular and is an internal bug.
    TINYDB_CHECK(pages_.size() == page_count, "sealing changed the frozen transaction page count");
    TINYDB_CHECK(resulting_state_.high_water_page_id == high_water, "sealing changed the frozen allocation frontier");
  }

  // Page LSNs and their checksums are the final bytes consumed by both WAL and
  // the prepared cache frames.
  for (auto &[page_id, page] : pages_) {
    if (!page->dirty || retired_page_ids_.contains(page_id)) {
      continue;
    }
    auto header = storage::RewriteDataPageLsn(std::as_writable_bytes(std::span<char, PAGE_SIZE>(*page->bytes)),
                                              page_id, commit_lsn);
    if (!header) {
      return std::move(header).error();
    }
    page->sealed_header = *header;
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
  return static_cast<std::size_t>(std::ranges::count_if(
      pages_, [this](const auto &entry) { return entry.second->dirty && !retired_page_ids_.contains(entry.first); }));
}

auto TransactionPages::ResultingState() const -> const DatabaseState & {
  TINYDB_CHECK(frozen_, "reading transaction state before freeze");
  return resulting_state_;
}

auto TransactionPages::TakePages() -> std::vector<cache::CommittedPageImage> {
  TINYDB_CHECK(sealed_, "taking transaction pages before seal");
  RequireUnpinned();
  auto result = std::vector<cache::CommittedPageImage>{};
  result.reserve(pages_.size());
  // Ownership moves directly toward publication; page bytes are not copied.
  for (auto &[page_id, page] : pages_) {
    if (!page->dirty || retired_page_ids_.contains(page_id)) {
      continue;
    }
    TINYDB_CHECK(page->sealed_header.page_id == page_id, "taking a page without its seal proof");
    result.push_back(cache::CommittedPageImage{
        .header = page->sealed_header,
        .bytes = std::move(page->bytes),
        .tree_payload_validated = page->tree_payload_validated,
    });
  }
  // One order serves the WAL digest, adjacent duplicate validation, and cache
  // publication. The unordered private map no longer needs a second borrowed
  // page list solely to impose determinism.
  std::ranges::sort(result, {}, [](const cache::CommittedPageImage &image) { return image.header.page_id; });
  // Any clean private copy served read-your-writes but has no committed image
  // to transfer. Clearing the map releases those copies with the dirty ones.
  pages_.clear();
  memory_used_bytes_ = 0;
  return result;
}

}  // namespace tinydb::txn
