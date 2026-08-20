#include "cache/direct_io_engine.h"

#include "io/direct_file.h"
#include "io/testable_posix.h"
#include "storage/disk_manager.h"
#include "util/check.h"

#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__NR_io_uring_enter) && defined(__NR_io_uring_register) && defined(__NR_io_uring_setup)
#define TINYDB_HAS_NATIVE_IO_URING 1
#endif
#endif

#ifndef TINYDB_HAS_NATIVE_IO_URING
#define TINYDB_HAS_NATIVE_IO_URING 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace tinydb::cache {

class DirectReadRunWake final {
 public:
  using Callback = void (*)(void *context) noexcept;

  DirectReadRunWake(void *context, Callback callback) : context_(context), callback_(callback) {}

  void Notify() noexcept {
    // Cancel can race with engine destruction. This lock prevents a callback
    // after Disable returns.
    auto lock = std::lock_guard(mutex_);
    if (callback_ != nullptr) {
      callback_(context_);
    }
  }

  void Disable() noexcept {
    auto lock = std::lock_guard(mutex_);
    context_ = nullptr;
    callback_ = nullptr;
  }

 private:
  std::mutex mutex_;
  void *context_;
  Callback callback_;
};

struct DirectReadRun::Impl final {
  Impl(std::vector<page_id_t> requested_page_ids, std::vector<PageArena::Lease> requested_pages,
       std::weak_ptr<DirectReadRunWake> requested_wake)
      : page_ids(std::move(requested_page_ids)), pages(std::move(requested_pages)), wake(std::move(requested_wake)) {}

  auto BeginLoading() noexcept -> bool {
    auto lock = std::lock_guard(mutex);
    if (state != DirectReadRunState::Queued) {
      return false;
    }
    if (cancellation_requested) {
      ReleasePagesLocked();
      PublishLocked(DirectReadRunState::Cancelled);
      return false;
    }
    PublishLocked(DirectReadRunState::Loading);
    return true;
  }

  void Complete(const Status &status) noexcept {
    auto lock = std::lock_guard(mutex);
    if (state == DirectReadRunState::Cancelled) {
      return;
    }
    if (state == DirectReadRunState::Queued) {
      ReleasePagesLocked();
      PublishLocked(status.Code() == StatusCode::Closed ? DirectReadRunState::Cancelled : DirectReadRunState::Failed);
      return;
    }
    TINYDB_CHECK(state == DirectReadRunState::Loading, "completing a non-loading exact direct-read run");
    // The final CQE ends kernel access to every destination in this run.
    // Only now can cancellation release the arena leases.
    if (cancellation_requested) {
      ReleasePagesLocked();
      PublishLocked(DirectReadRunState::Cancelled);
    } else if (status.Ok()) {
      PublishLocked(DirectReadRunState::Ready);
    } else {
      ReleasePagesLocked();
      PublishLocked(DirectReadRunState::Failed);
    }
  }

  void Cancel() noexcept {
    auto notify_reactor = false;
    {
      auto lock = std::lock_guard(mutex);
      cancellation_requested = true;
      notify_reactor = state == DirectReadRunState::Queued || state == DirectReadRunState::Loading;
      // Queued and ready pages are not kernel-owned. A loading run keeps its
      // pages until Complete receives all CQEs.
      if (state == DirectReadRunState::Queued || state == DirectReadRunState::Ready) {
        ReleasePagesLocked();
        PublishLocked(DirectReadRunState::Cancelled);
      }
    }
    if (notify_reactor) {
      if (const auto notifier = wake.lock(); notifier != nullptr) {
        notifier->Notify();
      }
    }
  }

  auto State() const noexcept -> DirectReadRunState {
    auto lock = std::lock_guard(mutex);
    return state;
  }

  auto Wait() noexcept -> DirectReadRunState {
    auto lock = std::unique_lock(mutex);
    changed.wait(lock, [this] { return state != DirectReadRunState::Queued && state != DirectReadRunState::Loading; });
    return state;
  }

  auto TakePage(std::size_t index) noexcept -> PageArena::Lease {
    auto lock = std::lock_guard(mutex);
    if (state != DirectReadRunState::Ready || index >= pages.size() || !pages[index]) {
      return {};
    }
    return std::move(pages[index]);
  }

  void PublishLocked(DirectReadRunState next) noexcept {
    state = next;
    changed.notify_all();
  }

  void ReleasePagesLocked() noexcept { pages.clear(); }

  const std::vector<page_id_t> page_ids;
  std::vector<PageArena::Lease> pages;
  std::weak_ptr<DirectReadRunWake> wake;
  mutable std::mutex mutex;
  std::condition_variable changed;
  DirectReadRunState state{DirectReadRunState::Queued};
  bool cancellation_requested{false};
};

namespace {

constexpr auto MAXIMUM_QUEUED_OPERATIONS = std::size_t{256};
constexpr auto MAXIMUM_ACTIVE_OPERATIONS = std::size_t{64};
constexpr auto MAXIMUM_IN_FLIGHT_SQES = std::size_t{64};
constexpr auto MAXIMUM_ACTIVE_WRITES = std::size_t{16};
constexpr auto RING_ENTRIES = std::uint32_t{256};
constexpr auto WAKE_USER_DATA = std::uint64_t{0};
constexpr auto INCOMPLETE_RESULT = std::numeric_limits<int>::min();

auto SegmentPageCount(std::span<const page_id_t> page_ids, std::size_t first_page) noexcept -> std::size_t {
  TINYDB_CHECK(first_page < page_ids.size(), "segmenting an absent direct-read page");
  auto page_count = std::size_t{1};
  while (page_count < io::MAX_DIRECT_READ_PAGES_PER_SQE && first_page + page_count < page_ids.size()) {
    const auto previous = page_ids[first_page + page_count - 1U];
    if (previous == std::numeric_limits<page_id_t>::max() || page_ids[first_page + page_count] != previous + 1U) {
      break;
    }
    ++page_count;
  }
  return page_count;
}

auto SubmissionCount(std::span<const page_id_t> page_ids) noexcept -> std::size_t {
  auto submissions = std::size_t{0};
  for (auto first_page = std::size_t{0}; first_page < page_ids.size();) {
    first_page += SegmentPageCount(page_ids, first_page);
    ++submissions;
  }
  return submissions;
}

/*
** An exact read completes through its shared run, whose own mutex and
** condition already serve every waiter, so the operation carries no
** synchronization of its own. A checkpoint write is waited on directly by
** the scheduling thread and therefore owns one-shot completion state.
*/
struct ExactReadOperation final {
  ExactReadOperation(std::shared_ptr<DirectReadRun::Impl> requested_run, io::DirectReadRequest request)
      : run(std::move(requested_run)),
        destination(reinterpret_cast<std::byte *>(run->pages.front().Bytes().data())),
        prepared(std::move(request)) {
    const auto valid = !run->page_ids.empty() && run->pages.size() == run->page_ids.size() && destination != nullptr;
    TINYDB_CHECK(valid, "constructing an invalid exact direct-read operation");
  }

  auto FirstPageId() const noexcept -> page_id_t { return run->page_ids.front(); }
  auto PageCount() const noexcept -> std::size_t { return run->page_ids.size(); }
  void Complete(const Status &status) noexcept { run->Complete(status); }

  std::shared_ptr<DirectReadRun::Impl> run;
  std::byte *destination{nullptr};
  std::optional<io::DirectReadRequest> prepared;
};

struct CheckpointWriteOperation final {
  CheckpointWriteOperation(page_id_t write_page_id, std::span<const DirectIoCheckpointPage> pages)
      : first_page_id(write_page_id), page_count(pages.size()) {
    const auto valid = !pages.empty() && pages.size() <= io::MAX_DIRECT_WRITE_BATCH_PAGES;
    TINYDB_CHECK(valid, "constructing an invalid direct checkpoint operation");
    for (auto index = std::size_t{0}; index < pages.size(); ++index) {
      vectors[index] = iovec{
          .iov_base = const_cast<char *>(pages[index].data),
          .iov_len = PAGE_SIZE,
      };
    }
  }

  void Complete(Status completion_status) {
    auto lock = std::lock_guard(mutex);
    TINYDB_CHECK(!done, "completing one direct checkpoint write twice");
    prepared.reset();
    status = std::move(completion_status);
    done = true;
    changed.notify_one();
  }

  auto Wait() -> Status {
    auto lock = std::unique_lock(mutex);
    changed.wait(lock, [this] { return done; });
    return std::move(status);
  }

  page_id_t first_page_id;
  std::size_t page_count;
  page_id_t captured_logical_page_count{FIRST_DATA_PAGE_ID};
  std::array<struct iovec, io::MAX_DIRECT_WRITE_BATCH_PAGES> vectors{};
  std::optional<io::DirectWriteRequest> prepared;
  std::mutex mutex;
  std::condition_variable changed;
  Status status;
  bool done{false};
};

template <typename Operation, std::size_t Capacity>
class OperationQueue final {
 public:
  auto Size() const noexcept -> std::size_t { return size_; }
  auto Empty() const noexcept -> bool { return size_ == 0; }

  void Push(std::shared_ptr<Operation> operation) noexcept {
    TINYDB_CHECK(operation != nullptr, "queuing an empty direct-I/O operation");
    TINYDB_CHECK(size_ < Capacity, "native direct-I/O queue is full");
    entries_[(first_ + size_) % Capacity] = std::move(operation);
    ++size_;
  }

  auto Pop() noexcept -> std::shared_ptr<Operation> {
    TINYDB_CHECK(size_ != 0, "popping an empty native direct-I/O queue");
    auto operation = std::move(entries_[first_]);
    first_ = (first_ + 1U) % Capacity;
    --size_;
    return operation;
  }

 private:
  std::array<std::shared_ptr<Operation>, Capacity> entries_{};
  std::size_t first_{0};
  std::size_t size_{0};
};

#if TINYDB_HAS_NATIVE_IO_URING

/*
** Only the reactor accesses these mappings. The atomic operations below
** synchronize the reactor with the kernel, not with API threads.
*/
class NativeRing final {
 public:
  struct Completion final {
    bool wake{false};
    std::size_t slot_index{0};
    std::size_t submission_index{0};
    int result{0};
  };

  struct Submission final {
    std::size_t count{0};
    int error{0};
  };

  static auto Create() noexcept -> std::unique_ptr<NativeRing> {
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN) && \
    defined(IORING_SETUP_COOP_TASKRUN) && defined(IORING_SETUP_SUBMIT_ALL)
    constexpr auto preferred_flags = std::array<std::uint32_t, 4>{
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL,
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL,
        IORING_SETUP_SUBMIT_ALL,
        0U,
    };
#else
    constexpr auto preferred_flags = std::array<std::uint32_t, 1>{0U};
#endif
    for (const auto flags : preferred_flags) {
      auto parameters = io_uring_params{};
      parameters.flags = flags;
      const auto descriptor = static_cast<int>(::syscall(__NR_io_uring_setup, RING_ENTRIES, &parameters));
      if (descriptor < 0) {
        continue;
      }
      try {
        auto ring = std::unique_ptr<NativeRing>(new NativeRing(descriptor, parameters));
        if (!ring->Map()) {
          return {};
        }
        return ring;
      } catch (const std::bad_alloc &) {
        ::close(descriptor);
        return {};
      }
    }
    return {};
  }

  NativeRing(const NativeRing &) = delete;
  auto operator=(const NativeRing &) -> NativeRing & = delete;

  ~NativeRing() {
    if (fixed_file_) {
      static_cast<void>(::syscall(__NR_io_uring_register, descriptor_, IORING_UNREGISTER_FILES, nullptr, 0U));
    }
    if (sqes_ != MAP_FAILED) {
      ::munmap(sqes_, sqes_bytes_);
    }
    if (single_mapping_) {
      if (sq_ring_ != MAP_FAILED) {
        ::munmap(sq_ring_, std::max(sq_ring_bytes_, cq_ring_bytes_));
      }
    } else {
      if (sq_ring_ != MAP_FAILED) {
        ::munmap(sq_ring_, sq_ring_bytes_);
      }
      if (cq_ring_ != MAP_FAILED) {
        ::munmap(cq_ring_, cq_ring_bytes_);
      }
    }
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  void QueueWakePoll(int wake_descriptor) noexcept {
    const auto can_queue = wake_descriptor >= 0 && AvailableEntries() != 0;
    TINYDB_CHECK(can_queue, "native ring cannot queue its wake poll");
    auto &entry = NextEntry();
    entry.opcode = IORING_OP_POLL_ADD;
    entry.fd = wake_descriptor;
    entry.poll_events = POLLIN;
    entry.user_data = WAKE_USER_DATA;
  }

  auto QueueRead(const io::DirectReadRequest &request, std::span<const page_id_t> page_ids, std::byte *destination,
                 std::size_t slot_index, std::span<std::size_t> expected_bytes) noexcept -> std::size_t {
    const auto submission_count = SubmissionCount(page_ids);
    const auto can_queue = destination != nullptr && submission_count != 0 &&
                           submission_count <= expected_bytes.size() && submission_count <= AvailableEntries();
    TINYDB_CHECK(can_queue, "native ring cannot queue direct read");
    TryRegisterFile(request.Descriptor());

    auto submission = std::size_t{0};
    for (auto first_page = std::size_t{0}; first_page < page_ids.size(); ++submission) {
      const auto page_count = SegmentPageCount(page_ids, first_page);
      const auto byte_count = page_count * PAGE_SIZE;
      auto &entry = NextEntry();
      entry.opcode = IORING_OP_READ;
      entry.fd = fixed_file_ ? 0 : request.Descriptor();
      entry.flags = fixed_file_ ? IOSQE_FIXED_FILE : 0;
      entry.off = page_ids[first_page] * PAGE_SIZE;
      entry.addr = reinterpret_cast<std::uint64_t>(destination + first_page * PAGE_SIZE);
      entry.len = static_cast<std::uint32_t>(byte_count);
      // Zero identifies the wake poll. These one-based fields identify the
      // active slot and its SQE within that slot.
      entry.user_data =
          (static_cast<std::uint64_t>(slot_index + 1U) << 32U) | static_cast<std::uint64_t>(submission + 1U);
      expected_bytes[submission] = byte_count;
      first_page += page_count;
    }
    TINYDB_CHECK(submission == submission_count, "direct read segmentation changed during queueing");
    return submission_count;
  }

  void QueueWrite(const io::DirectWriteRequest &request, page_id_t first_page_id, std::span<const struct iovec> vectors,
                  std::size_t slot_index) noexcept {
    const auto can_queue = !vectors.empty() && AvailableEntries() != 0;
    TINYDB_CHECK(can_queue, "native ring cannot queue checkpoint write");
    TryRegisterFile(request.Descriptor());
    auto &entry = NextEntry();
    entry.opcode = IORING_OP_WRITEV;
    entry.fd = fixed_file_ ? 0 : request.Descriptor();
    entry.flags = fixed_file_ ? IOSQE_FIXED_FILE : 0;
    entry.off = first_page_id * PAGE_SIZE;
    entry.addr = reinterpret_cast<std::uint64_t>(vectors.data());
    entry.len = static_cast<std::uint32_t>(vectors.size());
    entry.user_data = (static_cast<std::uint64_t>(slot_index + 1U) << 32U) | 1U;
  }

  auto SubmitAll() noexcept -> Submission {
    // io_uring_enter can accept only a prefix. Continue until the kernel
    // consumes the published tail or reports a terminal error.
    const auto target = __atomic_load_n(sq_tail_, __ATOMIC_ACQUIRE);
    const auto initial_head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    auto head = initial_head;
    while (head != target) {
      const auto remaining = target - head;
      const auto submitted =
          static_cast<int>(::syscall(__NR_io_uring_enter, descriptor_, remaining, 0U, 0U, nullptr, std::size_t{0}));
      if (submitted < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EBUSY) {
          continue;
        }
        return Submission{.count = static_cast<std::size_t>(head - initial_head), .error = errno};
      }
      head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    }
    return Submission{.count = static_cast<std::size_t>(target - initial_head)};
  }

  void DiscardPending() noexcept {
    // The kernel owns entries before its head. Remove only the unaccepted
    // suffix from the userspace queue.
    const auto head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    __atomic_store_n(sq_tail_, head, __ATOMIC_RELEASE);
  }

  auto TryPop(Completion *result) noexcept -> bool {
    TINYDB_CHECK(result != nullptr, "polling a native completion into a null destination");
    // The acquired tail makes the CQE visible. The released head returns the
    // copied slot to the kernel.
    const auto head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
    const auto tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
    if (head == tail) {
      return false;
    }
    const auto completion = cqes_[head & *cq_mask_];
    if (completion.user_data == WAKE_USER_DATA) {
      *result = Completion{.wake = true, .result = completion.res};
    } else {
      const auto encoded_slot = static_cast<std::uint32_t>(completion.user_data >> 32U);
      const auto encoded_submission = static_cast<std::uint32_t>(completion.user_data);
      const auto known_completion = encoded_slot != 0 && encoded_submission != 0;
      TINYDB_CHECK(known_completion, "native ring returned an unknown direct-I/O completion");
      *result = Completion{
          .wake = false,
          .slot_index = static_cast<std::size_t>(encoded_slot - 1U),
          .submission_index = static_cast<std::size_t>(encoded_submission - 1U),
          .result = completion.res,
      };
    }
    __atomic_store_n(cq_head_, head + 1U, __ATOMIC_RELEASE);
    return true;
  }

  auto WaitForCompletion() const noexcept -> int {
    for (;;) {
      // The acquired tail makes a new CQE visible. Only this reactor advances
      // the head, so its local availability check can use a relaxed load.
      if (__atomic_load_n(cq_head_, __ATOMIC_RELAXED) != __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE)) {
        return 0;
      }
      const auto result = static_cast<int>(
          ::syscall(__NR_io_uring_enter, descriptor_, 0U, 1U, IORING_ENTER_GETEVENTS, nullptr, std::size_t{0}));
      if (result >= 0 || errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return errno;
    }
  }

 private:
  NativeRing(int descriptor, io_uring_params parameters) : descriptor_(descriptor), parameters_(parameters) {}

  template <typename Value>
  static auto At(void *mapping, std::uint32_t offset) noexcept -> Value * {
    return reinterpret_cast<Value *>(static_cast<std::byte *>(mapping) + offset);
  }

  auto AvailableEntries() const noexcept -> std::size_t {
    // Acquire the kernel head before the reactor reuses an SQ slot.
    const auto head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    const auto tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);
    return static_cast<std::size_t>(*sq_entries_ - (tail - head));
  }

  void TryRegisterFile(int descriptor) noexcept {
    if (file_registration_attempted_) {
      const auto same_file = !fixed_file_ || descriptor == registered_descriptor_;
      TINYDB_CHECK(same_file, "native ring changed its registered database file");
      return;
    }
    file_registration_attempted_ = true;
    const auto result =
        static_cast<int>(::syscall(__NR_io_uring_register, descriptor_, IORING_REGISTER_FILES, &descriptor, 1U));
    if (result == 0) {
      fixed_file_ = true;
      registered_descriptor_ = descriptor;
    }
  }

  auto NextEntry() noexcept -> io_uring_sqe & {
    const auto tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);
    const auto index = tail & *sq_mask_;
    auto &entry = sqes_array_[index];
    std::memset(&entry, 0, sizeof(entry));
    sq_array_[index] = index;
    // Publish the initialized SQE only after its payload and array index are
    // visible to the kernel.
    __atomic_store_n(sq_tail_, tail + 1U, __ATOMIC_RELEASE);
    return entry;
  }

  auto Map() noexcept -> bool {
    sq_ring_bytes_ = static_cast<std::size_t>(parameters_.sq_off.array) +
                     static_cast<std::size_t>(parameters_.sq_entries) * sizeof(std::uint32_t);
    cq_ring_bytes_ = static_cast<std::size_t>(parameters_.cq_off.cqes) +
                     static_cast<std::size_t>(parameters_.cq_entries) * sizeof(io_uring_cqe);
    single_mapping_ = (parameters_.features & IORING_FEAT_SINGLE_MMAP) != 0;
    if (single_mapping_) {
      const auto bytes = std::max(sq_ring_bytes_, cq_ring_bytes_);
      sq_ring_ =
          ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, descriptor_, IORING_OFF_SQ_RING);
      if (sq_ring_ == MAP_FAILED) {
        return false;
      }
      cq_ring_ = sq_ring_;
    } else {
      sq_ring_ = ::mmap(nullptr, sq_ring_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, descriptor_,
                        IORING_OFF_SQ_RING);
      if (sq_ring_ == MAP_FAILED) {
        return false;
      }
      cq_ring_ = ::mmap(nullptr, cq_ring_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, descriptor_,
                        IORING_OFF_CQ_RING);
      if (cq_ring_ == MAP_FAILED) {
        return false;
      }
    }
    sqes_bytes_ = static_cast<std::size_t>(parameters_.sq_entries) * sizeof(io_uring_sqe);
    sqes_ =
        ::mmap(nullptr, sqes_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, descriptor_, IORING_OFF_SQES);
    if (sqes_ == MAP_FAILED) {
      return false;
    }
    sq_head_ = At<std::uint32_t>(sq_ring_, parameters_.sq_off.head);
    sq_tail_ = At<std::uint32_t>(sq_ring_, parameters_.sq_off.tail);
    sq_mask_ = At<std::uint32_t>(sq_ring_, parameters_.sq_off.ring_mask);
    sq_entries_ = At<std::uint32_t>(sq_ring_, parameters_.sq_off.ring_entries);
    sq_array_ = At<std::uint32_t>(sq_ring_, parameters_.sq_off.array);
    sqes_array_ = static_cast<io_uring_sqe *>(sqes_);
    cq_head_ = At<std::uint32_t>(cq_ring_, parameters_.cq_off.head);
    cq_tail_ = At<std::uint32_t>(cq_ring_, parameters_.cq_off.tail);
    cq_mask_ = At<std::uint32_t>(cq_ring_, parameters_.cq_off.ring_mask);
    cqes_ = At<io_uring_cqe>(cq_ring_, parameters_.cq_off.cqes);
    return true;
  }

  int descriptor_{-1};
  io_uring_params parameters_{};
  void *sq_ring_{MAP_FAILED};
  void *cq_ring_{MAP_FAILED};
  void *sqes_{MAP_FAILED};
  std::size_t sq_ring_bytes_{0};
  std::size_t cq_ring_bytes_{0};
  std::size_t sqes_bytes_{0};
  bool single_mapping_{false};
  std::uint32_t *sq_head_{nullptr};
  std::uint32_t *sq_tail_{nullptr};
  std::uint32_t *sq_mask_{nullptr};
  std::uint32_t *sq_entries_{nullptr};
  std::uint32_t *sq_array_{nullptr};
  io_uring_sqe *sqes_array_{nullptr};
  std::uint32_t *cq_head_{nullptr};
  std::uint32_t *cq_tail_{nullptr};
  std::uint32_t *cq_mask_{nullptr};
  io_uring_cqe *cqes_{nullptr};
  int registered_descriptor_{-1};
  bool file_registration_attempted_{false};
  bool fixed_file_{false};
};

#endif

}  // namespace
struct DirectIoEngine::Impl final {
  struct ActiveSlot final {
    // A slot owns its operation and transfer token while the kernel can access
    // their buffers. One logical read can produce several CQEs. Exactly one of
    // read and write is set while the slot is occupied.
    std::shared_ptr<ExactReadOperation> read;
    std::shared_ptr<CheckpointWriteOperation> write;
    std::optional<io::DirectReadRequest> read_request;
    std::optional<io::DirectWriteRequest> write_request;
    std::array<int, MAXIMUM_IN_FLIGHT_SQES> completion_results{};
    std::array<std::size_t, MAXIMUM_IN_FLIGHT_SQES> expected_bytes{};
    std::size_t completion_count{0};
    std::size_t completions_received{0};

    auto Occupied() const noexcept -> bool { return read != nullptr || write != nullptr; }
  };

  struct DispatchItem final {
    std::size_t slot_index;
    std::size_t submission_count;
  };

  struct DispatchBatch final {
    std::array<DispatchItem, MAXIMUM_ACTIVE_OPERATIONS> items{};
    std::size_t count{0};
  };

  explicit Impl(DiskManager *database)
      : disk(database), exact_read_wake(std::make_shared<DirectReadRunWake>(this, WakeExactRead)) {
    TINYDB_CHECK(disk != nullptr, "constructing a direct-I/O engine without a disk manager");
#if TINYDB_HAS_NATIVE_IO_URING
    if (!disk->UsesDirectIo()) {
      initialized = true;
      backend_failed = true;
      return;
    }
    wake_descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_descriptor < 0) {
      initialized = true;
      backend_failed = true;
      return;
    }
    try {
      reactor = std::thread([this] { Reactor(); });
    } catch (const std::system_error &) {
      ::close(wake_descriptor);
      wake_descriptor = -1;
      initialized = true;
      backend_failed = true;
      return;
    }
    auto lock = std::unique_lock(mutex);
    state_changed.wait(lock, [this] { return initialized; });
    const auto setup_failed = backend_failed;
    lock.unlock();
    if (setup_failed && reactor.joinable()) {
      reactor.join();
    }
    if (setup_failed && wake_descriptor >= 0) {
      ::close(wake_descriptor);
      wake_descriptor = -1;
    }
#else
    initialized = true;
    backend_failed = true;
#endif
  }

  ~Impl() {
    exact_read_wake->Disable();
    Stop();
#if TINYDB_HAS_NATIVE_IO_URING
    if (wake_descriptor >= 0) {
      ::close(wake_descriptor);
    }
#endif
  }

  auto QueueSizeLocked() const noexcept -> std::size_t { return exact_reads.Size() + writes.Size(); }

  auto AvailableLocked() const noexcept -> bool { return initialized && !backend_failed && !stopping; }

  static void WakeExactRead(void *context) noexcept {
    auto *const engine = static_cast<Impl *>(context);
    TINYDB_CHECK(engine != nullptr, "waking an absent direct-I/O engine");
#if TINYDB_HAS_NATIVE_IO_URING
    engine->Signal();
#else
    engine->state_changed.notify_all();
#endif
  }

  void CompleteQueuedLocked(const Status &status) {
    while (!exact_reads.Empty()) {
      exact_reads.Pop()->Complete(status);
    }
    while (!writes.Empty()) {
      writes.Pop()->Complete(status);
    }
    state_changed.notify_all();
  }

#if TINYDB_HAS_NATIVE_IO_URING
  void Signal() noexcept {
    // This atomic only combines eventfd writes. The engine mutex publishes
    // queued operations and state changes.
    if (wake_descriptor < 0 || wake_pending.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    constexpr auto increment = std::uint64_t{1};
    for (;;) {
      const auto written = ::write(wake_descriptor, &increment, sizeof(increment));
      if (written == static_cast<ssize_t>(sizeof(increment)) || (written < 0 && errno == EAGAIN)) {
        return;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      wake_pending.store(false, std::memory_order_release);
      return;
    }
  }

  void DrainWake() noexcept {
    auto value = std::uint64_t{0};
    for (;;) {
      const auto received = ::read(wake_descriptor, &value, sizeof(value));
      if (received == static_cast<ssize_t>(sizeof(value))) {
        // A concurrent enqueue is visible to this dispatch or writes the next
        // eventfd count after this store.
        wake_pending.store(false, std::memory_order_release);
        return;
      }
      if (received < 0 && errno == EINTR) {
        continue;
      }
      wake_pending.store(false, std::memory_order_release);
      return;
    }
  }

  auto FindFreeSlotLocked() const noexcept -> std::size_t {
    for (auto index = std::size_t{0}; index < active.size(); ++index) {
      if (!active[index].Occupied()) {
        return index;
      }
    }
    return active.size();
  }

  auto DispatchLocked(NativeRing &ring) -> DispatchBatch {
    auto batch = DispatchBatch{};
    while (active_count < MAXIMUM_ACTIVE_OPERATIONS && active_sqes < MAXIMUM_IN_FLIGHT_SQES) {
      // Keep one checkpoint write active so it can make progress. After that,
      // exact reads have priority. Extra writes use depth only when reads are
      // absent.
      const auto write_needs_progress = !writes.Empty() && active_writes == 0;
      const auto write_can_fill_idle_depth =
          exact_reads.Empty() && !writes.Empty() && active_writes < MAXIMUM_ACTIVE_WRITES;
      if (write_needs_progress || write_can_fill_idle_depth) {
        auto operation = writes.Pop();
        const auto slot_index = FindFreeSlotLocked();
        TINYDB_CHECK(slot_index != active.size(), "direct-I/O engine lost an active slot");
        auto &slot = active[slot_index];
        // The active slot owns the operation and its request token. Together,
        // they keep the file state and transfer memory valid until completion.
        std::ranges::fill(slot.completion_results, INCOMPLETE_RESULT);
        const auto vectors = std::span<const struct iovec>{operation->vectors}.first(operation->page_count);
        TINYDB_CHECK(operation->prepared.has_value(), "queued checkpoint write has no prepared request");
        slot.write_request = std::move(operation->prepared);
        slot.completion_count = 1;
        slot.expected_bytes[0] = operation->page_count * PAGE_SIZE;
        ring.QueueWrite(slot.write_request.value(), operation->first_page_id, vectors, slot_index);
        slot.write = std::move(operation);
        batch.items[batch.count++] = DispatchItem{.slot_index = slot_index, .submission_count = 1};
        ++active_count;
        ++active_sqes;
        ++active_writes;
        continue;
      }
      if (exact_reads.Empty()) {
        break;
      }

      auto operation = exact_reads.Pop();
      const auto needed_submissions = SubmissionCount(operation->run->page_ids);
      if (needed_submissions > MAXIMUM_IN_FLIGHT_SQES - active_sqes) {
        // A read run publishes its pages as one unit. Keep its SQEs together
        // until enough ring depth becomes free.
        exact_reads.Push(std::move(operation));
        break;
      }
      if (!operation->run->BeginLoading()) {
        operation->Complete(Status::Closed("exact direct-read run was cancelled before transfer"));
        continue;
      }

      const auto slot_index = FindFreeSlotLocked();
      TINYDB_CHECK(slot_index != active.size(), "direct-I/O engine lost an active slot");
      auto &slot = active[slot_index];
      std::ranges::fill(slot.completion_results, INCOMPLETE_RESULT);
      TINYDB_CHECK(operation->prepared.has_value(), "queued direct read has no prepared request");
      slot.read_request = std::move(operation->prepared);
      slot.completion_count = ring.QueueRead(slot.read_request.value(), operation->run->page_ids,
                                             operation->destination, slot_index, slot.expected_bytes);
      TINYDB_CHECK(slot.completion_count == needed_submissions, "direct-I/O request changed its submission count");
      slot.read = std::move(operation);
      batch.items[batch.count++] = DispatchItem{
          .slot_index = slot_index,
          .submission_count = needed_submissions,
      };
      ++active_count;
      active_sqes += needed_submissions;
    }
    state_changed.notify_all();
    return batch;
  }

  auto RecordCompletion(const NativeRing::Completion &completion) -> bool {
    // This function records one CQE but does not publish an operation result.
    // FinishSlot publishes the result after the last CQE.
    auto lock = std::lock_guard(mutex);
    const auto known_slot = completion.slot_index < active.size() && active[completion.slot_index].Occupied();
    TINYDB_CHECK(known_slot, "native ring completed an unknown direct-I/O slot");
    auto &slot = active[completion.slot_index];
    const auto new_completion = completion.submission_index < slot.completion_count &&
                                slot.completion_results[completion.submission_index] == INCOMPLETE_RESULT;
    TINYDB_CHECK(new_completion, "native ring duplicated a direct-I/O completion");
    slot.completion_results[completion.submission_index] = completion.result;
    ++slot.completions_received;
    return slot.completions_received == slot.completion_count;
  }

  void FinishSlot(std::size_t slot_index) {
    auto read = std::shared_ptr<ExactReadOperation>{};
    auto write = std::shared_ptr<CheckpointWriteOperation>{};
    auto read_request = std::optional<io::DirectReadRequest>{};
    auto write_request = std::optional<io::DirectWriteRequest>{};
    auto completion_results = std::array<int, MAXIMUM_IN_FLIGHT_SQES>{};
    auto expected_bytes = std::array<std::size_t, MAXIMUM_IN_FLIGHT_SQES>{};
    auto completion_count = std::size_t{0};
    auto hook = CompletionHookForTest{nullptr};
    auto *hook_context = static_cast<void *>(nullptr);
    {
      // Remove the active slot before transport completion. finishing_count
      // prevents DrainForTesting from observing a false idle state.
      auto lock = std::lock_guard(mutex);
      const auto known_slot = slot_index < active.size() && active[slot_index].Occupied();
      TINYDB_CHECK(known_slot, "native ring completed an unknown direct-I/O slot");
      auto &slot = active[slot_index];
      const auto completed = slot.completion_count != 0 && slot.completions_received == slot.completion_count;
      TINYDB_CHECK(completed, "finishing an incomplete direct-I/O slot");
      read = std::move(slot.read);
      write = std::move(slot.write);
      read_request = std::move(slot.read_request);
      write_request = std::move(slot.write_request);
      completion_count = slot.completion_count;
      std::ranges::copy(slot.completion_results, completion_results.begin());
      std::ranges::copy(slot.expected_bytes, expected_bytes.begin());
      hook = completion_hook;
      hook_context = completion_hook_context;
      slot = ActiveSlot{};
      const auto active_counts_valid = active_count != 0 && completion_count <= active_sqes;
      TINYDB_CHECK(active_counts_valid, "direct-I/O active count underflow");
      --active_count;
      active_sqes -= completion_count;
      if (write != nullptr) {
        TINYDB_CHECK(active_writes != 0, "direct-I/O active write count underflow");
        --active_writes;
      }
      ++finishing_count;
      state_changed.notify_all();
    }

    const auto writing = write != nullptr;
    auto results = std::span<int>{completion_results}.first(completion_count);
    if (hook != nullptr) {
      const auto original = results.front();
      hook(hook_context, writing, writing ? write->first_page_id : read->FirstPageId(),
           writing ? write->page_count : read->PageCount(), &results.front());
      if (results.front() != original && results.size() != 1U) {
        std::ranges::fill(results, results.front());
      }
    }
    auto status = Status{};
    if (read_request.has_value()) {
      status = std::move(*read_request)
                   .Complete(results, std::span<const std::size_t>{expected_bytes}.first(completion_count));
    } else {
      TINYDB_CHECK(write_request.has_value(), "direct-I/O slot has no completion token");
      TINYDB_CHECK(completion_count == 1, "checkpoint write produced multiple completions");
      status = std::move(*write_request).Complete(results.front(), expected_bytes.front());
    }
    // Complete the transfer token before publishing the logical operation.
    // This order releases fork admission before pages become available.
    if (writing) {
      write->Complete(std::move(status));
    } else {
      read->Complete(status);
    }
    {
      auto lock = std::lock_guard(mutex);
      TINYDB_CHECK(finishing_count != 0, "direct-I/O finishing count underflow");
      --finishing_count;
      state_changed.notify_all();
    }
  }

  void FailUnsubmitted(const DispatchBatch &batch, std::size_t submitted_sqes, int error) {
    // An io_uring_enter failure can follow an accepted SQE prefix. Create
    // failures only for the unaccepted suffix. Accepted SQEs still need CQEs.
    auto completed_slots = std::array<std::size_t, MAXIMUM_ACTIVE_OPERATIONS>{};
    auto completed_count = std::size_t{0};
    {
      auto lock = std::lock_guard(mutex);
      auto remaining_submitted = submitted_sqes;
      for (auto index = std::size_t{0}; index < batch.count; ++index) {
        const auto item = batch.items[index];
        auto &slot = active[item.slot_index];
        const auto valid_item = slot.Occupied() && slot.completion_count == item.submission_count;
        TINYDB_CHECK(valid_item, "failing an invalid direct-I/O dispatch item");
        const auto accepted = std::min(remaining_submitted, item.submission_count);
        remaining_submitted -= accepted;
        for (auto submission = accepted; submission < item.submission_count; ++submission) {
          TINYDB_CHECK(slot.completion_results[submission] == INCOMPLETE_RESULT,
                       "failing an already completed direct-I/O submission");
          slot.completion_results[submission] = -error;
          ++slot.completions_received;
        }
        if (slot.completions_received == slot.completion_count) {
          completed_slots[completed_count++] = item.slot_index;
        }
      }
      TINYDB_CHECK(remaining_submitted == 0, "io_uring reported too many submitted SQEs");
      backend_failed = true;
      CompleteQueuedLocked(Status::IoError("io_uring submission failed: " + std::generic_category().message(error)));
    }
    for (auto index = std::size_t{0}; index < completed_count; ++index) {
      FinishSlot(completed_slots[index]);
    }
  }

  void FailAllActive(int error) {
    // The reactor closes the ring before this function. Thus, the kernel no
    // longer references any active slot.
    auto completed_slots = std::array<std::size_t, MAXIMUM_ACTIVE_OPERATIONS>{};
    auto completed_count = std::size_t{0};
    {
      auto lock = std::lock_guard(mutex);
      backend_failed = true;
      CompleteQueuedLocked(
          Status::IoError("io_uring completion wait failed: " + std::generic_category().message(error)));
      for (auto index = std::size_t{0}; index < active.size(); ++index) {
        auto &slot = active[index];
        if (!slot.Occupied()) {
          continue;
        }
        for (auto submission = std::size_t{0}; submission < slot.completion_count; ++submission) {
          if (slot.completion_results[submission] == INCOMPLETE_RESULT) {
            slot.completion_results[submission] = -error;
            ++slot.completions_received;
          }
        }
        TINYDB_CHECK(slot.completions_received == slot.completion_count,
                     "terminal ring failure left an incomplete slot");
        completed_slots[completed_count++] = index;
      }
    }
    for (auto index = std::size_t{0}; index < completed_count; ++index) {
      FinishSlot(completed_slots[index]);
    }
  }

  void Reactor() noexcept {
    auto native_ring = NativeRing::Create();
    if (native_ring == nullptr) {
      auto lock = std::lock_guard(mutex);
      initialized = true;
      backend_failed = true;
      state_changed.notify_all();
      return;
    }
    native_ring->QueueWakePoll(wake_descriptor);
    const auto initial = native_ring->SubmitAll();
    if (initial.error != 0 || initial.count != 1U) {
      auto lock = std::lock_guard(mutex);
      initialized = true;
      backend_failed = true;
      state_changed.notify_all();
      return;
    }
    {
      auto lock = std::lock_guard(mutex);
      initialized = true;
      state_changed.notify_all();
    }

    auto wake_armed = true;
    for (;;) {
      auto completion = NativeRing::Completion{};
      while (native_ring->TryPop(&completion)) {
        if (completion.wake) {
          DrainWake();
          wake_armed = false;
        } else if (RecordCompletion(completion)) {
          FinishSlot(completion.slot_index);
        }
      }

      auto batch = DispatchBatch{};
      auto queued_wake = false;
      auto should_stop = false;
      {
        auto lock = std::lock_guard(mutex);
        // Stop and failure reject queued work first. The reactor exits only
        // after every kernel-owned slot completes.
        should_stop = (stopping || backend_failed) && active_count == 0;
        if (!should_stop && !backend_failed) {
          if (!wake_armed) {
            native_ring->QueueWakePoll(wake_descriptor);
            wake_armed = true;
            queued_wake = true;
          }
          batch = DispatchLocked(*native_ring);
        }
      }
      if (should_stop) {
        break;
      }

      const auto submitted = native_ring->SubmitAll();
      if (submitted.error != 0) {
        native_ring->DiscardPending();
        // A rearmed wake poll precedes this data batch. Remove it before the
        // accepted prefix is mapped to data SQEs.
        const auto submitted_data =
            submitted.count > (queued_wake ? 1U : 0U) ? submitted.count - (queued_wake ? 1U : 0U) : 0U;
        const auto dispatched_sqes = [&] {
          auto total = std::size_t{0};
          for (auto index = std::size_t{0}; index < batch.count; ++index) {
            total += batch.items[index].submission_count;
          }
          return total;
        }();
        FailUnsubmitted(batch, std::min(submitted_data, dispatched_sqes), submitted.error);
        continue;
      }
      const auto wait_error = native_ring->WaitForCompletion();
      if (wait_error != 0) {
        // Closing the ring ends kernel ownership before synthetic completions
        // release the active buffers.
        native_ring.reset();
        FailAllActive(wait_error);
        break;
      }
    }

    auto lock = std::lock_guard(mutex);
    state_changed.notify_all();
  }
#endif

  auto ScheduleExact(std::vector<page_id_t> page_ids,
                     std::vector<PageArena::Lease> pages) noexcept -> std::shared_ptr<DirectReadRun::Impl> {
    if (io::TestHookInstalledForTesting() || page_ids.empty() || page_ids.size() > io::MAX_DIRECT_READ_BATCH_PAGES ||
        pages.size() != page_ids.size() ||
        std::ranges::any_of(page_ids, [](const auto page_id) { return page_id < FIRST_DATA_PAGE_ID; }) ||
        std::ranges::any_of(pages, [](const auto &page) { return !page; }) ||
        SubmissionCount(page_ids) > MAXIMUM_IN_FLIGHT_SQES) {
      return {};
    }
    const auto *const first_page = &pages.front().Bytes();
    for (auto index = std::size_t{1}; index < pages.size(); ++index) {
      if (&pages[index].Bytes() != first_page + index) {
        return {};
      }
    }
    {
      auto lock = std::lock_guard(mutex);
      if (!AvailableLocked() || QueueSizeLocked() >= MAXIMUM_QUEUED_OPERATIONS) {
        return {};
      }
    }

    try {
      auto run = std::make_shared<DirectReadRun::Impl>(std::move(page_ids), std::move(pages), exact_read_wake);
      auto destinations = std::as_writable_bytes(std::span<PageBytes>{&run->pages.front().Bytes(), run->pages.size()});
      auto request = disk->BeginDirectReadPages(run->page_ids, destinations);
      if (!request) {
        return {};
      }
      auto operation = std::make_shared<ExactReadOperation>(run, std::move(*request));
      {
        // Request preparation acquires fork admission. Recheck engine state
        // because Stop can race with preparation.
        auto lock = std::lock_guard(mutex);
        if (!AvailableLocked() || QueueSizeLocked() >= MAXIMUM_QUEUED_OPERATIONS) {
          return {};
        }
        exact_reads.Push(std::move(operation));
      }
#if TINYDB_HAS_NATIVE_IO_URING
      Signal();
#endif
      return run;
    } catch (const std::bad_alloc &) {
      return {};
    } catch (const std::length_error &) {
      return {};
    } catch (...) {
      return {};
    }
  }

  auto WriteCheckpointPages(std::span<const DirectIoCheckpointPage> pages,
                            page_id_t captured_logical_page_count) -> Result<bool> {
    if (pages.empty()) {
      return true;
    }
    for (auto index = std::size_t{0}; index < pages.size(); ++index) {
      if (pages[index].data == nullptr || pages[index].page_id < FIRST_DATA_PAGE_ID ||
          pages[index].page_id >= captured_logical_page_count ||
          (index != 0 && pages[index - 1U].page_id >= pages[index].page_id)) {
        return std::unexpected(Status::InvalidArgument("checkpoint engine received invalid or unordered pages"));
      }
    }
    if (io::TestHookInstalledForTesting()) {
      return false;
    }
    {
      auto lock = std::lock_guard(mutex);
      if (!initialized || backend_failed) {
        return false;
      }
      if (stopping) {
        return std::unexpected(Status::Closed("direct-I/O engine is stopping"));
      }
    }

    auto operations = std::vector<std::shared_ptr<CheckpointWriteOperation>>{};
    try {
      operations.reserve((pages.size() - 1U) / io::MAX_DIRECT_WRITE_BATCH_PAGES + 1U);
      for (auto first = std::size_t{0}; first < pages.size();) {
        auto end = first + 1U;
        while (end < pages.size() && end - first < io::MAX_DIRECT_WRITE_BATCH_PAGES &&
               pages[end].page_id == pages[end - 1U].page_id + 1U) {
          ++end;
        }
        auto operation =
            std::make_shared<CheckpointWriteOperation>(pages[first].page_id, pages.subspan(first, end - first));
        operation->captured_logical_page_count = captured_logical_page_count;
        operations.push_back(std::move(operation));
        first = end;
      }
    } catch (const std::bad_alloc &) {
      return std::unexpected(Status::ResourceExhausted("checkpoint direct-I/O scheduling allocation failed"));
    } catch (const std::length_error &) {
      return std::unexpected(Status::ResourceExhausted("checkpoint direct-I/O schedule is too large"));
    }

    auto queued = std::size_t{0};
    auto scheduling_failure = Status{};
    auto scheduling_failed = false;
    auto use_fallback = false;
    for (auto index = std::size_t{0}; index < operations.size(); ++index) {
      auto &operation = operations[index];
      const auto vectors = std::span<const struct iovec>{operation->vectors}.first(operation->page_count);
      auto request =
          disk->BeginDirectCheckpointWrite(operation->first_page_id, vectors, operation->captured_logical_page_count);
      if (!request) {
        scheduling_failure = std::move(request).error();
        scheduling_failed = true;
        break;
      }
      operation->prepared = std::move(*request);

      {
        auto lock = std::unique_lock(mutex);
        // Waiting here holds this request's fork-gate admission while the
        // queue drains. That only lengthens a concurrent fork's wait: the
        // reactor consumes the queue without acquiring admissions, so
        // capacity reappears and this admission is later released through
        // ordinary completion.
        state_changed.wait(
            lock, [this] { return stopping || backend_failed || QueueSizeLocked() < MAXIMUM_QUEUED_OPERATIONS; });
        if (stopping) {
          scheduling_failure = Status::Closed("direct-I/O engine stopped during checkpoint");
          scheduling_failed = true;
          break;
        }
        if (backend_failed) {
          // Synchronous fallback is safe only before the first batch enters
          // the reactor queue.
          use_fallback = queued == 0;
          scheduling_failure = Status::IoError("direct-I/O engine failed during checkpoint");
          scheduling_failed = true;
          break;
        }
        writes.Push(operation);
        ++queued;
      }
#if TINYDB_HAS_NATIVE_IO_URING
      Signal();
#endif
    }

    auto transfer_failure = Status{};
    auto transfer_failed = false;
    // Wait for every queued batch. A prior failure cannot make this function
    // return while a later write still uses a source pointer.
    for (auto index = std::size_t{0}; index < queued; ++index) {
      auto status = operations[index]->Wait();
      if (!status.Ok() && !transfer_failed) {
        transfer_failure = std::move(status);
        transfer_failed = true;
      }
    }
    if (transfer_failed) {
      return std::unexpected(std::move(transfer_failure));
    }
    if (use_fallback) {
      return false;
    }
    if (scheduling_failed) {
      return std::unexpected(std::move(scheduling_failure));
    }
    return true;
  }

  void DrainForTesting() {
    auto lock = std::unique_lock(mutex);
    state_changed.wait(lock, [this] { return QueueSizeLocked() == 0 && active_count == 0 && finishing_count == 0; });
  }

  auto Available() const noexcept -> bool {
    if (io::TestHookInstalledForTesting()) {
      return false;
    }
    auto lock = std::lock_guard(mutex);
    return AvailableLocked();
  }

  void SetCompletionHookForTest(CompletionHookForTest hook, void *context) noexcept {
    auto lock = std::lock_guard(mutex);
    completion_hook = hook;
    completion_hook_context = context;
  }

  void Stop() noexcept {
    // Complete queued operations first. Then join waits until the reactor
    // drains all kernel-owned slots.
    {
      auto lock = std::lock_guard(mutex);
      if (stopping) {
        return;
      }
      stopping = true;
      CompleteQueuedLocked(Status::Closed("direct-I/O engine stopped before transfer"));
    }
#if TINYDB_HAS_NATIVE_IO_URING
    Signal();
    if (reactor.joinable()) {
      reactor.join();
    }
#endif
  }

  DiskManager *disk;
  std::shared_ptr<DirectReadRunWake> exact_read_wake;
  // The mutex protects all queues, slots, counters, state flags, and test-hook
  // fields below. wake_pending only protects eventfd write coalescing.
  mutable std::mutex mutex;
  std::condition_variable state_changed;
  OperationQueue<ExactReadOperation, MAXIMUM_QUEUED_OPERATIONS> exact_reads;
  OperationQueue<CheckpointWriteOperation, MAXIMUM_QUEUED_OPERATIONS> writes;
  std::array<ActiveSlot, MAXIMUM_ACTIVE_OPERATIONS> active;
  std::size_t active_count{0};
  std::size_t active_sqes{0};
  std::size_t active_writes{0};
  std::size_t finishing_count{0};
  bool initialized{false};
  bool backend_failed{false};
  bool stopping{false};
  CompletionHookForTest completion_hook{nullptr};
  void *completion_hook_context{nullptr};
#if TINYDB_HAS_NATIVE_IO_URING
  std::thread reactor;
  int wake_descriptor{-1};
  std::atomic<bool> wake_pending{false};
#endif
};

DirectReadRun::~DirectReadRun() { Cancel(); }

auto DirectReadRun::State() const noexcept -> DirectReadRunState { return impl_->State(); }

auto DirectReadRun::Wait() noexcept -> DirectReadRunState { return impl_->Wait(); }

auto DirectReadRun::TakePage(std::size_t index) noexcept -> PageArena::Lease { return impl_->TakePage(index); }

void DirectReadRun::Cancel() noexcept { impl_->Cancel(); }

DirectIoEngine::DirectIoEngine(DiskManager *disk) : impl_(std::make_unique<Impl>(disk)) {}
DirectIoEngine::~DirectIoEngine() = default;

auto DirectIoEngine::ScheduleExact(std::vector<page_id_t> page_ids,
                                   std::vector<PageArena::Lease> pages) noexcept -> std::shared_ptr<DirectReadRun> {
  auto run = impl_->ScheduleExact(std::move(page_ids), std::move(pages));
  if (run == nullptr) {
    return {};
  }
  try {
    return std::shared_ptr<DirectReadRun>(new DirectReadRun(run));
  } catch (...) {
    // The internal run is already scheduled. Drain it before its public owner
    // disappears.
    run->Cancel();
    static_cast<void>(run->Wait());
    return {};
  }
}

auto DirectIoEngine::WriteCheckpointPages(std::span<const DirectIoCheckpointPage> pages,
                                          page_id_t captured_logical_page_count) -> Result<bool> {
  return impl_->WriteCheckpointPages(pages, captured_logical_page_count);
}

auto DirectIoEngine::Available() const noexcept -> bool { return impl_->Available(); }

void DirectIoEngine::DrainForTesting() { impl_->DrainForTesting(); }

void DirectIoEngine::SetCompletionHookForTest(CompletionHookForTest hook, void *context) noexcept {
  impl_->SetCompletionHookForTest(hook, context);
}

}  // namespace tinydb::cache
