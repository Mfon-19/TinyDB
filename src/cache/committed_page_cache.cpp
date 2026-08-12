#include "cache/committed_page_cache.h"

#include "cache/direct_io_engine.h"
#include "io/page_file.h"
#include "storage/disk_manager.h"
#include "util/check.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "storage/page_codec.h"

#include <array>
#include <condition_variable>
#include <expected>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace tinydb::cache {

/*
** A frame owns exactly one immutable encoded page version. Its validated
** common header orders physical versions and travels with immutable handles,
** so consumers never checksum the same bytes twice. The header LSN and cache
** checkpoint LSN determine eviction eligibility, not visibility.
*/
struct CommittedFrame final {
  CommittedFrame(storage::DataPageHeader initial_header, PageArena::Lease initial_bytes)
      : header(initial_header), bytes(std::move(initial_bytes)) {}

  storage::DataPageHeader header;  // checksum-authenticated common fields
  PageArena::Lease bytes;          // immutable after construction

  /*
  ** CHECKPOINTED LRU LINKS
  **
  ** Every current checkpointed frame belongs to this intrusive list, including
  ** a temporarily pinned one. newer points toward the MRU end and older toward
  ** the LRU end. Eviction skips pins; release therefore only drops shared
  ** ownership and never reacquires the cache mutex.
  */
  CommittedFrame *newer{nullptr};
  CommittedFrame *older{nullptr};
  bool in_lru{false};
};

struct LoadState final {
  enum class Phase {
    Loading,
    Ready,
    Failed,
  };

  std::condition_variable changed;
  Phase phase{Phase::Loading};
  std::shared_ptr<CommittedFrame> result;
  Status failure;
};

namespace {

auto IsTreePage(const storage::DataPageHeader &header) -> bool {
  return header.type == storage::DataPageType::Leaf || header.type == storage::DataPageType::Internal;
}

auto Lease(std::shared_ptr<CommittedFrame> frame) -> PageHandle {
  auto *const leased = frame.get();
  auto keeper = std::static_pointer_cast<const void>(std::move(frame));
  return {leased->header.page_id, leased->bytes->data(), std::move(keeper), &leased->header,
          IsTreePage(leased->header) ? PagePayloadProof::Tree : PagePayloadProof::None};
}

/*
** DIRECT READ STAGING
**
** A planned direct read stays outside the replacement cache. Demand can use a
** staged page without cache admission. Each staged page owns an arena lease
** and one shared budget reservation.
** The budget limits all streams to 4 MiB or one quarter of the cache target,
** whichever is smaller.
**
** Cancellation drains the native run before it releases page ownership and
** budget reservations. Thus, the kernel never uses a returned arena page.
*/
class StagingBudget final {
 public:
  explicit StagingBudget(std::size_t page_limit) : page_limit_(page_limit) {}

  auto ReserveUpTo(std::size_t requested_pages) noexcept -> std::size_t {
    auto lock = std::lock_guard(mutex_);
    const auto available_pages = page_limit_ - used_pages_;
    const auto reserved_pages = std::min(requested_pages, available_pages);
    used_pages_ += reserved_pages;
    return reserved_pages;
  }

  void Release(std::size_t page_count) noexcept {
    auto lock = std::lock_guard(mutex_);
    TINYDB_CHECK(page_count <= used_pages_, "direct read staging budget underflow");
    used_pages_ -= page_count;
  }

  auto Empty() const noexcept -> bool { return page_limit_ == 0; }

 private:
  const std::size_t page_limit_;
  std::mutex mutex_;
  std::size_t used_pages_{0};
};

/*
** A staged page owns one arena lease and one budget reservation. A consuming
** demand decodes the page and stores its validated header here before the
** object backs an immutable PageHandle.
*/
class StagedPage final {
 public:
  StagedPage() = default;
  StagedPage(PageArena::Lease bytes, std::shared_ptr<StagingBudget> budget)
      : bytes_(std::move(bytes)), budget_(std::move(budget)), reserved_(true) {}
  StagedPage(const StagedPage &) = delete;
  auto operator=(const StagedPage &) -> StagedPage & = delete;
  StagedPage(StagedPage &&other) noexcept
      : header(other.header),
        bytes_(std::move(other.bytes_)),
        budget_(std::move(other.budget_)),
        reserved_(std::exchange(other.reserved_, false)) {}
  auto operator=(StagedPage &&) -> StagedPage & = delete;
  ~StagedPage() { Reset(); }

  explicit operator bool() const noexcept { return static_cast<bool>(bytes_); }
  auto Bytes() noexcept -> PageBytes & { return bytes_.Bytes(); }

  storage::DataPageHeader header{};

 private:
  void Reset() noexcept {
    if (!reserved_) {
      return;
    }
    // Reset releases the page before its budget slot. Another plan cannot use
    // this capacity while the prior page still owns physical memory.
    bytes_ = {};
    budget_->Release(1);
    reserved_ = false;
  }

  PageArena::Lease bytes_;
  std::shared_ptr<StagingBudget> budget_;
  bool reserved_{false};
};

/*
** A StagedRun owns one native request and all unconsumed budget reservations.
** A successful demand transfers one page and its reservation to a PageHandle.
** Destruction cancels and drains the request before it releases the remainder.
*/
class StagedRun final {
 public:
  StagedRun(std::vector<page_id_t> page_ids, std::shared_ptr<DirectReadRun> run, std::shared_ptr<StagingBudget> budget,
            std::size_t reserved_pages)
      : page_ids_(std::move(page_ids)),
        run_(std::move(run)),
        budget_(std::move(budget)),
        reserved_pages_(reserved_pages) {}
  StagedRun(const StagedRun &) = delete;
  auto operator=(const StagedRun &) -> StagedRun & = delete;
  ~StagedRun() { CancelAndDrain(); }

  auto IndexOf(page_id_t page_id) const noexcept -> std::optional<std::size_t> {
    const auto found = std::ranges::find(page_ids_, page_id);
    if (found == page_ids_.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(found - page_ids_.begin());
  }

  auto Reusable() const noexcept -> bool {
    auto lock = std::lock_guard(mutex_);
    if (run_ == nullptr) {
      return false;
    }
    const auto state = run_->State();
    return state == DirectReadRunState::Queued || state == DirectReadRunState::Loading ||
           state == DirectReadRunState::Ready;
  }

  auto TakeReadyPage(std::size_t index) noexcept -> StagedPage {
    auto observed_run = std::shared_ptr<DirectReadRun>{};
    {
      auto lock = std::lock_guard(mutex_);
      if (run_ == nullptr || index >= page_ids_.size()) {
        return {};
      }
      observed_run = run_;
    }
    const auto completed_state = observed_run->Wait();

    auto completed_run = std::shared_ptr<DirectReadRun>{};
    auto released_pages = std::size_t{0};
    auto lock = std::unique_lock(mutex_);
    if (run_ != observed_run) {
      return {};
    }
    if (completed_state != DirectReadRunState::Ready) {
      completed_run = std::move(run_);
      released_pages = std::exchange(reserved_pages_, 0);
      lock.unlock();
      completed_run.reset();
      budget_->Release(released_pages);
      return {};
    }
    auto page = run_->TakePage(index);
    if (!page) {
      return {};
    }
    TINYDB_CHECK(reserved_pages_ != 0, "taking an unreserved direct read page");
    --reserved_pages_;
    return {std::move(page), budget_};
  }

  void CancelAndDrain() noexcept {
    auto run = std::shared_ptr<DirectReadRun>{};
    auto released_pages = std::size_t{0};
    {
      auto lock = std::lock_guard(mutex_);
      run = std::move(run_);
      released_pages = std::exchange(reserved_pages_, 0);
    }
    if (run != nullptr) {
      run->Cancel();
      static_cast<void>(run->Wait());
      run.reset();
    }
    budget_->Release(released_pages);
  }

 private:
  const std::vector<page_id_t> page_ids_;
  mutable std::mutex mutex_;
  std::shared_ptr<DirectReadRun> run_;
  std::shared_ptr<StagingBudget> budget_;
  std::size_t reserved_pages_;
};

struct ReadStreamState final {
  std::mutex mutex;
  std::vector<page_id_t> plan;
  std::vector<std::shared_ptr<StagedRun>> runs;
  // A generation change rejects an old plan after a new plan or
  // CloseReadStream.
  std::uint64_t generation{0};
  bool open{true};
};

void CancelRuns(const std::vector<std::shared_ptr<StagedRun>> &runs) noexcept {
  for (const auto &run : runs) {
    run->CancelAndDrain();
  }
}

}  // namespace

struct CommittedPageCache::Impl final {
  // The staging limit covers speculative pages outside the replacement cache.
  // Each staged run fills at most one maximal transport read, and one plan
  // holds at most two such runs.
  static constexpr auto MAXIMUM_STAGING_BYTES = std::size_t{4} * 1024U * 1024U;
  static constexpr auto MAXIMUM_PHYSICAL_RUN_PAGES = io::MAX_DIRECT_READ_BATCH_PAGES;
  static constexpr auto MAXIMUM_PLANNED_PAGES = 2U * MAXIMUM_PHYSICAL_RUN_PAGES;

  DiskManager &disk;  // backing checkpointed database file
  const std::size_t target_bytes;
  std::shared_ptr<PageArena> page_arena;
  mutable std::mutex mutex;  // protects every field below
  std::vector<std::shared_ptr<CommittedFrame>> pages;
  std::unordered_map<page_id_t, std::shared_ptr<LoadState>> loads;
  std::size_t resident_pages{0};
  std::size_t loading_pages{0};
  std::size_t dirty_pages{0};
  CommittedFrame *most_recent{nullptr};
  CommittedFrame *least_recent{nullptr};
  std::uint64_t checkpoint_lsn;  // newest page LSN stored in the database file
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t evictions{0};
  std::uint64_t read_ahead_plans{0};
  std::uint64_t read_ahead_pages_scheduled{0};
  std::uint64_t read_ahead_pages_consumed{0};
  std::shared_ptr<StagingBudget> staging_budget;
  // C++ destroys members in reverse declaration order. The reactor therefore
  // stops before load state, frames, and arena leases begin destruction.
  std::unique_ptr<DirectIoEngine> direct_io;

  // Buffered mode creates only a heap arena. Direct mode also creates the
  // shared staging budget and one native engine.
  Impl(DiskManager &database_file, std::size_t byte_target, std::uint64_t initial_checkpoint_lsn)
      : disk(database_file),
        target_bytes(byte_target),
        page_arena(database_file.UsesDirectIo() ? PageArena::CreateDirect(byte_target / PAGE_SIZE)
                                                : PageArena::CreateHeap()),
        checkpoint_lsn(initial_checkpoint_lsn),
        staging_budget(database_file.UsesDirectIo()
                           ? std::make_shared<StagingBudget>(
                                 std::min(MAXIMUM_STAGING_BYTES / PAGE_SIZE, byte_target / (4U * PAGE_SIZE)))
                           : nullptr),
        direct_io(database_file.UsesDirectIo() ? std::make_unique<DirectIoEngine>(&database_file) : nullptr) {}

  auto IsCheckpointed(const CommittedFrame &frame) const -> bool { return frame.header.page_lsn <= checkpoint_lsn; }

  void LinkMostRecent(CommittedFrame *frame) {
    TINYDB_CHECK(!frame->in_lru, "linking a page already present in the LRU");
    TINYDB_CHECK(frame->newer == nullptr, "linking a page with a stale newer LRU link");
    TINYDB_CHECK(frame->older == nullptr, "linking a page with a stale older LRU link");
    TINYDB_CHECK(IsCheckpointed(*frame), "linking an uncheckpointed eviction candidate");

    frame->older = most_recent;
    if (most_recent != nullptr) {
      most_recent->newer = frame;
    } else {
      least_recent = frame;
    }
    most_recent = frame;
    frame->in_lru = true;
  }

  void Unlink(CommittedFrame *frame) {
    TINYDB_CHECK(frame->in_lru, "unlinking a page absent from the LRU");
    if (frame->newer != nullptr) {
      frame->newer->older = frame->older;
    } else {
      TINYDB_CHECK(most_recent == frame, "eviction MRU link is inconsistent");
      most_recent = frame->older;
    }
    if (frame->older != nullptr) {
      frame->older->newer = frame->newer;
    } else {
      TINYDB_CHECK(least_recent == frame, "eviction LRU link is inconsistent");
      least_recent = frame->newer;
    }
    frame->newer = nullptr;
    frame->older = nullptr;
    frame->in_lru = false;
  }

  void Touch(CommittedFrame *frame) {
    if (frame == most_recent) {
      return;
    }
    Unlink(frame);
    LinkMostRecent(frame);
  }

  void RecordAccess(const std::shared_ptr<CommittedFrame> &frame) {
    if (frame->in_lru) {
      Touch(frame.get());
    }
  }

  auto TakeEvictionCandidate() -> std::shared_ptr<CommittedFrame> {
    // Pinned frames remain linked so their final release needs no mutex. In
    // ordinary use they are near the MRU end, leaving victim selection O(1);
    // a long-lived reader may require a short walk past its pinned frames.
    auto *victim = least_recent;
    while (victim != nullptr) {
      const auto page_id = victim->header.page_id;
      TINYDB_CHECK(page_id < pages.size(), "LRU page lies outside the cache table");
      TINYDB_CHECK(pages[page_id].get() == victim, "LRU contains a stale frame");
      if (pages[page_id].use_count() == 1) {
        break;
      }
      victim = victim->newer;
    }
    if (victim == nullptr) {
      return {};
    }
    const auto victim_page_id = victim->header.page_id;
    Unlink(victim);
    auto frame = std::move(pages[victim_page_id]);
    --resident_pages;
    ++evictions;
    return frame;
  }

  void TrimToTarget() {
    while (resident_pages + loading_pages > target_bytes / PAGE_SIZE) {
      if (TakeEvictionCandidate() == nullptr) {
        break;
      }
    }
  }

  auto MakeRoomForRead() -> std::shared_ptr<CommittedFrame> {
    auto reusable = std::shared_ptr<CommittedFrame>{};
    while (resident_pages + loading_pages >= target_bytes / PAGE_SIZE) {
      auto frame = TakeEvictionCandidate();
      if (frame == nullptr) {
        break;
      }
      if (reusable == nullptr) {
        reusable = std::move(frame);
      }
    }
    return reusable;
  }

  auto HasRoomForRead() const -> bool { return resident_pages + loading_pages < target_bytes / PAGE_SIZE; }

  /*
  ** Every blocking and page-sized operation in a physical miss happens here,
  ** outside mutex. An unobserved eviction victim supplies its existing page
  ** buffer and frame control block. Buffered mode has no arena free list, so
  ** this reuse is what prevents two allocations per steady-state miss; the
  ** miss-per-read benchmark loses about 6% without it.
  */
  auto LoadFrame(page_id_t page_id,
                 std::shared_ptr<CommittedFrame> frame) const -> Result<std::shared_ptr<CommittedFrame>> {
    try {
      auto new_bytes = PageArena::Lease{};
      if (frame == nullptr) {
        new_bytes = page_arena->Acquire();
        if (!new_bytes) {
          return std::unexpected(Status::ResourceExhausted("committed page load allocation failed"));
        }
      }
      auto *const bytes = frame != nullptr ? &frame->bytes.Bytes() : &new_bytes.Bytes();
      auto destination = std::as_writable_bytes(std::span{*bytes});
      auto status = disk.ReadPage(page_id, destination);
      if (!status.Ok()) {
        if (status.Code() == StatusCode::InvalidArgument) {
          return std::unexpected(Status::Corruption("tree references a page outside the allocated database"));
        }
        return std::unexpected(std::move(status));
      }
      const auto header =
          storage::DecodeDataPageHeader(std::as_bytes(std::span<const char, PAGE_SIZE>(*bytes)), page_id);
      if (!header) {
        return std::unexpected(header.error());
      }
      if (IsTreePage(*header)) {
        if (auto validation_status = ValidateTreePagePayload(bytes->data(), *header); !validation_status.Ok()) {
          return std::unexpected(std::move(validation_status));
        }
      }

      if (frame != nullptr) {
        frame->header = *header;
      } else {
        frame = std::make_shared<CommittedFrame>(*header, std::move(new_bytes));
      }
      return frame;
    } catch (const std::bad_alloc &) {
      return std::unexpected(Status::ResourceExhausted("committed page load allocation failed"));
    }
  }

  // The caller holds mutex. The completed state retains one frame pin until
  // every same-page waiter has copied it into its PageHandle.
  void CompleteLoad(const std::shared_ptr<LoadState> &load, page_id_t page_id,
                    Result<std::shared_ptr<CommittedFrame>> loaded) {
    const auto active_load = load != nullptr && load->phase == LoadState::Phase::Loading;
    TINYDB_CHECK(active_load, "completing an inactive committed-page load");
    TINYDB_CHECK(loading_pages != 0, "committed-page loading count underflow");
    const auto active = loads.find(page_id);
    TINYDB_CHECK(active != loads.end() && active->second == load, "committed-page load lost its coalescing slot");
    --loading_pages;

    if (page_id < pages.size() && pages[page_id] != nullptr) {
      // Publication may install a newer immutable version while physical I/O
      // is outside the mutex. As before, that visible version wins even if the
      // redundant disk read failed.
      load->result = pages[page_id];
      RecordAccess(load->result);
      load->phase = LoadState::Phase::Ready;
    } else if (!loaded) {
      load->failure = std::move(loaded.error());
      load->phase = LoadState::Phase::Failed;
    } else {
      if (page_id >= pages.size()) {
        try {
          pages.resize(page_id + 1U);
        } catch (const std::bad_alloc &) {
          load->failure = Status::ResourceExhausted("committed cache page table allocation failed");
          load->phase = LoadState::Phase::Failed;
        } catch (const std::length_error &) {
          load->failure = Status::ResourceExhausted("committed cache page table exceeds container limits");
          load->phase = LoadState::Phase::Failed;
        }
      }
      if (load->phase == LoadState::Phase::Loading) {
        auto &current = pages[page_id];
        current = std::move(*loaded);
        ++resident_pages;
        LinkMostRecent(current.get());
        load->result = current;
        RecordAccess(load->result);
        load->phase = LoadState::Phase::Ready;
      }
    }
    loads.erase(active);
    load->changed.notify_all();
  }

  auto Read(page_id_t page_id) -> Result<PageHandle> {
    auto load = std::shared_ptr<LoadState>{};
    auto reusable = std::shared_ptr<CommittedFrame>{};
    auto loader = false;
    {
      auto lock = std::unique_lock(mutex);
      if (page_id < pages.size() && pages[page_id] != nullptr) {
        ++hits;
        RecordAccess(pages[page_id]);
        return Lease(pages[page_id]);
      }
      ++misses;

      if (const auto active = loads.find(page_id); active != loads.end()) {
        load = active->second;
      } else {
        try {
          load = std::make_shared<LoadState>();
        } catch (const std::bad_alloc &) {
          return std::unexpected(Status::ResourceExhausted("committed-page load-state allocation failed"));
        }

        // A loading page consumes the same hard capacity as a resident page.
        // Publication alone may temporarily exceed the target with dirty pages.
        reusable = MakeRoomForRead();
        if (!HasRoomForRead()) {
          return std::unexpected(
              Status::ResourceExhausted("committed cache is full of pinned, loading, or uncheckpointed pages"));
        }
        try {
          const auto [position, inserted] = loads.emplace(page_id, load);
          (void)position;
          TINYDB_CHECK(inserted, "committed-page load was reserved twice");
        } catch (const std::bad_alloc &) {
          return std::unexpected(Status::ResourceExhausted("committed-page load table allocation failed"));
        } catch (const std::length_error &) {
          return std::unexpected(Status::ResourceExhausted("committed-page load table exceeds container limits"));
        }
        ++loading_pages;
        loader = true;
      }
    }

    if (loader) {
      auto loaded = LoadFrame(page_id, std::move(reusable));
      auto lock = std::lock_guard(mutex);
      CompleteLoad(load, page_id, std::move(loaded));
    }

    auto lock = std::unique_lock(mutex);
    load->changed.wait(lock, [&] { return load->phase != LoadState::Phase::Loading; });
    if (load->phase == LoadState::Phase::Failed) {
      return std::unexpected(load->failure);
    }
    TINYDB_CHECK(load->result != nullptr, "completed committed-page load has no frame");
    RecordAccess(load->result);
    return Lease(load->result);
  }

  void PrimeExactPages(const std::shared_ptr<ReadStreamState> &state,
                       std::span<const page_id_t> hinted_page_ids) noexcept {
    // Staged pages intentionally remain outside the replacement cache. This
    // keeps speculative advice from changing cache residency or displacing a
    // demanded page. A later ordinary demand uses the replacement cache.
    try {
      const auto copied_count = std::min(hinted_page_ids.size(), MAXIMUM_PLANNED_PAGES);
      const auto copied_hints = std::vector<page_id_t>{
          hinted_page_ids.begin(), hinted_page_ids.begin() + static_cast<std::ptrdiff_t>(copied_count)};
      auto plan = std::vector<page_id_t>{};
      plan.reserve(std::min(copied_hints.size(), MAXIMUM_PLANNED_PAGES));
      const auto logical_page_count = disk.LogicalPageCount();
      {
        auto lock = std::lock_guard(mutex);
        for (const auto page_id : copied_hints) {
          if (page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count ||
              (page_id < pages.size() && pages[page_id] != nullptr) || loads.contains(page_id) ||
              std::ranges::find(plan, page_id) != plan.end()) {
            continue;
          }
          plan.push_back(page_id);
          if (plan.size() == MAXIMUM_PLANNED_PAGES) {
            break;
          }
        }
      }

      auto old_runs = std::vector<std::shared_ptr<StagedRun>>{};
      auto generation = std::uint64_t{0};
      {
        auto lock = std::lock_guard(state->mutex);
        if (!state->open) {
          return;
        }
        const auto reusable_plan = state->plan == plan && !state->runs.empty() &&
                                   std::ranges::all_of(state->runs, [](const auto &run) { return run->Reusable(); });
        if (reusable_plan) {
          return;
        }
        // The generation changes before cancellation can wait for native I/O.
        // A concurrent close or new plan then wins the comparison.
        generation = ++state->generation;
        state->plan.clear();
        old_runs.swap(state->runs);
      }
      CancelRuns(old_runs);

      auto new_runs = std::vector<std::shared_ptr<StagedRun>>{};
      auto scheduled_plan = std::vector<page_id_t>{};
      new_runs.reserve((plan.size() + MAXIMUM_PHYSICAL_RUN_PAGES - 1U) / MAXIMUM_PHYSICAL_RUN_PAGES);
      scheduled_plan.reserve(plan.size());
      for (auto first = std::size_t{0}; first < plan.size();) {
        const auto requested_pages = std::min(MAXIMUM_PHYSICAL_RUN_PAGES, plan.size() - first);
        auto run_page_ids = std::vector<page_id_t>{plan.begin() + static_cast<std::ptrdiff_t>(first),
                                                   plan.begin() + static_cast<std::ptrdiff_t>(first + requested_pages)};
        auto engine_page_ids = run_page_ids;
        auto leases = std::vector<PageArena::Lease>(requested_pages);
        const auto reserved_pages = staging_budget->ReserveUpTo(requested_pages);
        if (reserved_pages == 0) {
          break;
        }
        run_page_ids.resize(reserved_pages);
        engine_page_ids.resize(reserved_pages);
        leases.resize(reserved_pages);
        if (!page_arena->AcquireBatch(leases)) {
          staging_budget->Release(reserved_pages);
          break;
        }
        auto direct_run = direct_io->ScheduleExact(std::move(engine_page_ids), std::move(leases));
        if (direct_run == nullptr) {
          staging_budget->Release(reserved_pages);
          break;
        }
        auto staged_run = std::shared_ptr<StagedRun>{};
        try {
          staged_run = std::make_shared<StagedRun>(run_page_ids, direct_run, staging_budget, reserved_pages);
        } catch (...) {
          direct_run->Cancel();
          static_cast<void>(direct_run->Wait());
          direct_run.reset();
          staging_budget->Release(reserved_pages);
          throw;
        }
        scheduled_plan.insert(scheduled_plan.end(), run_page_ids.begin(), run_page_ids.end());
        new_runs.push_back(std::move(staged_run));
        first += reserved_pages;
        if (reserved_pages != requested_pages) {
          break;
        }
      }

      const auto scheduled_pages = scheduled_plan.size();
      auto discard = false;
      {
        auto lock = std::lock_guard(state->mutex);
        discard = !state->open || state->generation != generation;
        if (!discard) {
          state->plan = std::move(scheduled_plan);
          state->runs.swap(new_runs);
        }
      }
      if (discard) {
        CancelRuns(new_runs);
      } else if (scheduled_pages != 0) {
        auto lock = std::lock_guard(mutex);
        ++read_ahead_plans;
        read_ahead_pages_scheduled += scheduled_pages;
      }
    } catch (...) {
      // Advice is optional. Allocation and scheduling failure must not change
      // the result of a later semantic Read.
      return;
    }
  }

  // Staging is only an optimization. Any race, cancellation, I/O error,
  // validation error, or allocation error uses the ordinary coalesced read.
  auto ReadStreamPage(const std::shared_ptr<ReadStreamState> &state, page_id_t page_id) -> Result<PageHandle> {
    auto staged_run = std::shared_ptr<StagedRun>{};
    auto staged_index = std::size_t{0};
    if (state != nullptr) {
      auto lock = std::lock_guard(state->mutex);
      if (state->open) {
        for (const auto &run : state->runs) {
          const auto index = run->IndexOf(page_id);
          if (index.has_value()) {
            staged_run = run;
            staged_index = *index;
            break;
          }
        }
      }
    }
    if (staged_run == nullptr) {
      return Read(page_id);
    }

    auto join_active_load = false;
    {
      auto lock = std::lock_guard(mutex);
      if (page_id < pages.size() && pages[page_id] != nullptr) {
        ++hits;
        RecordAccess(pages[page_id]);
        return Lease(pages[page_id]);
      }
      join_active_load = loads.contains(page_id);
    }
    if (join_active_load) {
      return Read(page_id);
    }

    auto staged = staged_run->TakeReadyPage(staged_index);
    if (!staged) {
      return Read(page_id);
    }
    const auto encoded = std::as_bytes(std::span<const char, PAGE_SIZE>{staged.Bytes()});
    const auto decoded = storage::DecodeDataPageHeader(encoded, page_id);
    if (!decoded) {
      return Read(page_id);
    }
    auto payload_proof = PagePayloadProof::None;
    if (IsTreePage(*decoded)) {
      if (auto status = ValidateTreePagePayload(staged.Bytes().data(), *decoded); !status.Ok()) {
        return Read(page_id);
      }
      payload_proof = PagePayloadProof::Tree;
    }
    staged.header = *decoded;

    auto staged_page = std::shared_ptr<StagedPage>{};
    try {
      staged_page = std::make_shared<StagedPage>(std::move(staged));
    } catch (const std::bad_alloc &) {
      return Read(page_id);
    }
    {
      auto lock = std::lock_guard(mutex);
      if (page_id < pages.size() && pages[page_id] != nullptr) {
        ++hits;
        RecordAccess(pages[page_id]);
        return Lease(pages[page_id]);
      }
      if (loads.contains(page_id)) {
        // Join the ordinary coalesced load outside the cache mutex.
      } else {
        ++misses;
        ++read_ahead_pages_consumed;
        auto *const page = staged_page.get();
        auto keepalive = std::static_pointer_cast<const void>(std::move(staged_page));
        return PageHandle(page_id, page->Bytes().data(), std::move(keepalive), &page->header, payload_proof);
      }
    }
    return Read(page_id);
  }

  static void CloseReadStream(const std::shared_ptr<ReadStreamState> &state) noexcept {
    auto runs = std::vector<std::shared_ptr<StagedRun>>{};
    {
      auto lock = std::lock_guard(state->mutex);
      if (!state->open) {
        return;
      }
      state->open = false;
      ++state->generation;
      state->plan.clear();
      runs = std::move(state->runs);
    }
    CancelRuns(runs);
  }
};

CommittedPageCache::CommittedPageCache(DiskManager &disk, std::size_t target_bytes, std::uint64_t checkpoint_lsn)
    : impl_(std::make_unique<Impl>(disk, target_bytes, checkpoint_lsn)) {}

CommittedPageCache::~CommittedPageCache() = default;

auto CommittedPageCache::SharedPageArena() const noexcept -> std::shared_ptr<PageArena> { return impl_->page_arena; }

auto CommittedPageCache::Read(page_id_t page_id) -> Result<PageHandle> { return impl_->Read(page_id); }

auto CommittedPageCache::BeginReadStream() -> PageReadStream {
  if (impl_->direct_io == nullptr || !impl_->direct_io->Available() || impl_->staging_budget->Empty()) {
    // This synchronous stream preserves all read semantics without staging.
    return PageReader::BeginReadStream();
  }
  auto state = std::shared_ptr<ReadStreamState>{};
  try {
    state = std::make_shared<ReadStreamState>();
  } catch (const std::bad_alloc &) {
    return PageReader::BeginReadStream();
  }
  const auto read = [](void *owner, const std::shared_ptr<void> &opaque_state,
                       page_id_t page_id) -> Result<PageHandle> {
    return static_cast<Impl *>(owner)->ReadStreamPage(std::static_pointer_cast<ReadStreamState>(opaque_state), page_id);
  };
  const auto prime = [](void *owner, const std::shared_ptr<void> &opaque_state,
                        std::span<const page_id_t> page_ids) noexcept {
    static_cast<Impl *>(owner)->PrimeExactPages(std::static_pointer_cast<ReadStreamState>(opaque_state), page_ids);
  };
  const auto close = [](void *, const std::shared_ptr<void> &opaque_state) noexcept {
    Impl::CloseReadStream(std::static_pointer_cast<ReadStreamState>(opaque_state));
  };
  return {impl_.get(), std::move(state), read, prime, close};
}

auto CommittedPageCache::PreparePublication(std::vector<CommittedPageImage> images, std::vector<page_id_t> retired,
                                            page_id_t logical_page_count) -> Result<PublicationPlan> {
  if (logical_page_count < FIRST_DATA_PAGE_ID) {
    return std::unexpected(Status::InvalidArgument("publication has an invalid logical page count"));
  }

  auto frames = std::vector<std::shared_ptr<CommittedFrame>>{};
  frames.reserve(images.size());

  /*
  ** PREPARE CACHE OWNERSHIP
  **
  ** Grow the dense table and allocate every shared control block here. The
  ** writer has not appended WAL yet, so any validation or allocation failure
  ** remains a definite abort.
  */
  auto lock = std::lock_guard(impl_->mutex);
  if (impl_->pages.size() < logical_page_count) {
    impl_->pages.resize(logical_page_count);
  }
  auto previous_page_id = HEADER_PAGE_ID;
  for (auto &image : images) {
    const auto &header = image.header;
    if (!image.bytes || header.page_id < FIRST_DATA_PAGE_ID || header.page_id >= logical_page_count ||
        header.page_id <= previous_page_id || header.page_lsn == 0 ||
        header.payload_bytes > PAGE_SIZE - storage::data_page_offset::HEADER_BYTES) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid or duplicate page image"));
    }
    previous_page_id = header.page_id;
    const auto tree_page = IsTreePage(header);
    const auto non_tree_page =
        header.type == storage::DataPageType::Allocator || header.type == storage::DataPageType::Overflow;
    if (!tree_page && !non_tree_page) {
      return std::unexpected(Status::InvalidArgument("publication contains an unknown page type"));
    }
    if (tree_page && !image.tree_payload_validated) {
      if (auto status = ValidateTreePagePayload(image.bytes->data(), header); !status.Ok()) {
        return std::unexpected(std::move(status));
      }
    }
    if (!tree_page && image.tree_payload_validated) {
      return std::unexpected(Status::InvalidArgument("non-tree publication carries a tree validation proof"));
    }
    const auto &existing = impl_->pages[header.page_id];
    if (existing != nullptr && existing->header.page_lsn > header.page_lsn) {
      return std::unexpected(Status::InvalidArgument("committed page version moves backward"));
    }
    frames.push_back(std::make_shared<CommittedFrame>(header, std::move(image.bytes)));
  }
  previous_page_id = HEADER_PAGE_ID;
  auto image_index = std::size_t{0};
  for (const auto page_id : retired) {
    while (image_index < images.size() && images[image_index].header.page_id < page_id) {
      ++image_index;
    }
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= logical_page_count || page_id <= previous_page_id ||
        (image_index < images.size() && images[image_index].header.page_id == page_id)) {
      return std::unexpected(Status::InvalidArgument("publication contains an invalid retired page"));
    }
    previous_page_id = page_id;
  }
  return PublicationPlan(std::move(frames), std::move(retired));
}

void CommittedPageCache::Publish(PublicationPlan plan) noexcept {
  auto lock = std::lock_guard(impl_->mutex);

  for (const auto page_id : plan.retired_) {
    if (impl_->pages[page_id] == nullptr) {
      continue;
    }
    if (impl_->pages[page_id]->in_lru) {
      impl_->Unlink(impl_->pages[page_id].get());
    }
    impl_->dirty_pages -= !impl_->IsCheckpointed(*impl_->pages[page_id]) ? 1U : 0U;
    impl_->pages[page_id].reset();
    --impl_->resident_pages;
  }
  for (auto &frame : plan.frames_) {
    auto &current = impl_->pages[frame->header.page_id];
    if (current == nullptr) {
      ++impl_->resident_pages;
    } else {
      impl_->dirty_pages -= !impl_->IsCheckpointed(*current) ? 1U : 0U;
      if (current->in_lru) {
        impl_->Unlink(current.get());
      }
    }
    current = std::move(frame);
    impl_->dirty_pages += !impl_->IsCheckpointed(*current) ? 1U : 0U;
    if (impl_->IsCheckpointed(*current)) {
      impl_->LinkMostRecent(current.get());
    }
  }
  impl_->TrimToTarget();
}

void CommittedPageCache::MarkCheckpointed(std::uint64_t checkpoint_lsn) {
  {
    auto lock = std::lock_guard(impl_->mutex);
    const auto previous_checkpoint_lsn = impl_->checkpoint_lsn;
    impl_->checkpoint_lsn = checkpoint_lsn;

    // The database file now contains versions at or before checkpoint_lsn.
    // Later committed versions remain WAL-backed and non-evictable.
    for (const auto &page : impl_->pages) {
      if (page != nullptr && page->header.page_lsn > previous_checkpoint_lsn &&
          page->header.page_lsn <= checkpoint_lsn) {
        --impl_->dirty_pages;
        if (!page->in_lru) {
          impl_->LinkMostRecent(page.get());
        }
      }
    }
    impl_->TrimToTarget();
  }
  impl_->page_arena->ReleaseFreeMemory();
}

auto CommittedPageCache::CaptureDirtyPages() -> std::vector<PageHandle> {
  auto lock = std::lock_guard(impl_->mutex);
  // Return guards in page-ID order. A later publication may replace a table
  // entry, but the guard keeps this exact old immutable version alive until
  // checkpoint I/O finishes.
  auto result = std::vector<PageHandle>{};
  result.reserve(impl_->dirty_pages);
  for (const auto &page : impl_->pages) {
    if (page != nullptr && !impl_->IsCheckpointed(*page)) {
      result.push_back(Lease(page));
    }
  }
  return result;
}

auto CommittedPageCache::WriteCheckpointPages(std::span<const PageHandle> pages,
                                              page_id_t captured_logical_page_count) -> Status {
  // CaptureDirtyPages produces ordered non-null page handles. The native
  // engine and DiskManager validate the batches they receive against their
  // own transfer bounds, so this method adds no third copy of those checks.
  if (impl_->direct_io != nullptr) {
    auto direct_pages = std::vector<DirectIoCheckpointPage>{};
    try {
      direct_pages.reserve(pages.size());
      for (const auto &page : pages) {
        direct_pages.push_back(DirectIoCheckpointPage{.page_id = page.Id(), .data = page.Data()});
      }
    } catch (const std::bad_alloc &) {
      return Status::ResourceExhausted("checkpoint cache page-list allocation failed");
    } catch (const std::length_error &) {
      return Status::ResourceExhausted("checkpoint cache page list exceeds container limits");
    }
    auto native = impl_->direct_io->WriteCheckpointPages(direct_pages, captured_logical_page_count);
    if (!native) {
      // An error can follow a partial native transfer. The checkpoint reports
      // that error and does not use the synchronous fallback.
      return std::move(native.error());
    }
    if (*native) {
      return {};
    }
    // False means that no native write started. The synchronous path can write
    // the complete page set without a repeated native transfer.
  }

  // The synchronous path groups only consecutive page IDs. The fixed array
  // bounds each request for buffered and direct transports.
  auto batch = std::array<const std::byte *, io::MAX_PAGE_WRITE_BATCH_PAGES>{};
  auto batch_first = page_id_t{0};
  auto batch_size = std::size_t{0};
  const auto flush_batch = [&]() -> Status {
    if (batch_size == 0) {
      return {};
    }
    const auto status = impl_->disk.WriteCheckpointPages(
        batch_first, std::span<const std::byte *const>{batch.data(), batch_size}, captured_logical_page_count);
    batch_size = 0;
    return status;
  };
  for (const auto &page : pages) {
    const auto continues = batch_size != 0 && page.Id() == batch_first + batch_size;
    if (batch_size == batch.size() || (batch_size != 0 && !continues)) {
      if (auto status = flush_batch(); !status.Ok()) {
        return status;
      }
    }
    if (batch_size == 0) {
      batch_first = page.Id();
    }
    batch[batch_size++] = reinterpret_cast<const std::byte *>(page.Data());
  }
  return flush_batch();
}

void CommittedPageCache::DrainIoForTesting() {
  if (impl_->direct_io != nullptr) {
    impl_->direct_io->DrainForTesting();
  }
}

auto CommittedPageCache::DirtyPages() const -> std::size_t {
  auto lock = std::lock_guard(impl_->mutex);
  return impl_->dirty_pages;
}

auto CommittedPageCache::Stats() const -> CommittedCacheStats {
  auto lock = std::lock_guard(impl_->mutex);
  auto pinned_pages = std::size_t{0};
  for (const auto &page : impl_->pages) {
    if (page == nullptr) {
      continue;
    }
    pinned_pages += page.use_count() > 1 ? 1U : 0U;
  }
  return CommittedCacheStats{
      .target_bytes = impl_->target_bytes,
      .resident_bytes = impl_->resident_pages * PAGE_SIZE,
      .resident_pages = impl_->resident_pages,
      .pinned_pages = pinned_pages,
      .dirty_pages = impl_->dirty_pages,
      .hits = impl_->hits,
      .misses = impl_->misses,
      .evictions = impl_->evictions,
      .read_ahead_plans = impl_->read_ahead_plans,
      .read_ahead_pages_scheduled = impl_->read_ahead_pages_scheduled,
      .read_ahead_pages_consumed = impl_->read_ahead_pages_consumed,
  };
}

}  // namespace tinydb::cache
