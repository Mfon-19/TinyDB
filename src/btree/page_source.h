#pragma once

#include <tinydb/status.h>
#include "storage/page.h"
#include "util/check.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace tinydb {

namespace storage {
struct DataPageHeader;
}

/*
** A private page may defer its checksum until commit after its builder has
** proved the page-local type-specific payload. MutableData clears that proof,
** so code cannot continue relying on a proof established for older bytes.
*/
enum class PagePayloadProof : std::uint8_t {
  None,
  Tree,
  Overflow,
};

/*
** PAGE ACCESS BOUNDARY
**
** Tree mutation uses Read, Edit, Allocate, and Free without knowing whether
** bytes came from the committed cache, a write-transaction overlay, or an
** in-memory test model. Traversal can additionally request optional read
** advice through PageReadStream.
** This boundary keeps replacement policy, physical I/O, and copy-on-write
** ownership out of the B+ tree algorithms; all implementations must expose
** the same page identity and lease rules instead.
**
** Read, Edit, and Allocate return a move-only PageHandle. While the handle
** lives, its byte address and page identity are stable. A source may not reuse
** or destroy the underlying page until the handle releases its lease. Read
** handles are immutable. Edit and Allocate handles accumulate a sticky dirty
** bit that is returned to their source exactly once.
**
** The opaque owner and function pointer type-erase release without allocating
** a wrapper on the ordinary read path.
*/
class PageHandle {
 public:
  using Release = void (*)(void *owner, page_id_t page_id, bool dirty, PagePayloadProof payload_proof);

  PageHandle() = default;
  // Mutable typing is the construction-time proof that an editable handle
  // cannot be created over truly const storage. The member becomes const-only
  // so all later writes still pass through MutableData().
  // NOLINTNEXTLINE(readability-non-const-parameter)
  PageHandle(void *owner, page_id_t page_id, char *data, bool editable, Release release,
             PagePayloadProof payload_proof = PagePayloadProof::None)
      : owner_(owner),
        page_id_(page_id),
        data_(data),
        release_(release),
        editable_(editable),
        payload_proof_(payload_proof) {}

  // The keepalive retains whatever immutable storage backs data. For a cache
  // frame this shared owner is also an eviction pin; a staged direct-I/O page
  // uses it to retain its arena lease and staging reservation.
  PageHandle(page_id_t page_id, const char *data, std::shared_ptr<const void> keepalive,
             const storage::DataPageHeader *validated_header = nullptr,
             PagePayloadProof payload_proof = PagePayloadProof::None)
      : page_id_(page_id),
        data_(data),
        keepalive_(std::move(keepalive)),
        validated_header_(validated_header),
        payload_proof_(payload_proof) {}

  PageHandle(const PageHandle &) = delete;
  auto operator=(const PageHandle &) -> PageHandle & = delete;

  PageHandle(PageHandle &&other) noexcept { Swap(other); }

  auto operator=(PageHandle &&other) noexcept -> PageHandle & {
    if (this != &other) {
      auto replacement = PageHandle(std::move(other));
      Swap(replacement);
    }
    return *this;
  }

  ~PageHandle() {
    if (release_ != nullptr) {
      release_(owner_, page_id_, dirty_, payload_proof_);
    }
  }

  auto Id() const -> page_id_t { return page_id_; }
  auto Data() const -> const char * { return data_; }

  // Immutable cache owners may retain the common-header proof produced when
  // bytes first crossed the persistent validation boundary. Transaction
  // handles may instead carry a structural proof for their private bytes.
  auto ValidatedHeader() const noexcept -> const storage::DataPageHeader * { return validated_header_; }
  auto PayloadProof() const noexcept -> PagePayloadProof { return payload_proof_; }
  auto TreePayloadValidated() const noexcept -> bool { return payload_proof_ == PagePayloadProof::Tree; }
  auto OverflowPayloadValidated() const noexcept -> bool { return payload_proof_ == PagePayloadProof::Overflow; }

  auto MutableData() -> char * {
    TINYDB_CHECK(editable_, "mutable access through a read-only page handle");
    // Any write invalidates the previous structural proof. The responsible
    // builder or codec restores it after emitting one canonical payload.
    payload_proof_ = PagePayloadProof::None;
    return const_cast<char *>(data_);
  }

  void MarkTreePayloadValidated() {
    TINYDB_CHECK(editable_, "marking a read-only page as structurally validated");
    payload_proof_ = PagePayloadProof::Tree;
  }

  void MarkOverflowPayloadValidated() {
    TINYDB_CHECK(editable_, "marking a read-only overflow page as structurally validated");
    payload_proof_ = PagePayloadProof::Overflow;
  }

  void MarkDirty() {
    TINYDB_CHECK(editable_, "marking a read-only page handle dirty");
    dirty_ = true;
  }

 private:
  void Swap(PageHandle &other) noexcept {
    using std::swap;
    swap(owner_, other.owner_);
    swap(page_id_, other.page_id_);
    swap(data_, other.data_);
    swap(release_, other.release_);
    swap(keepalive_, other.keepalive_);
    swap(validated_header_, other.validated_header_);
    swap(editable_, other.editable_);
    swap(dirty_, other.dirty_);
    swap(payload_proof_, other.payload_proof_);
  }

  void *owner_{nullptr};
  page_id_t page_id_{HEADER_PAGE_ID};
  const char *data_{nullptr};
  Release release_{nullptr};
  std::shared_ptr<const void> keepalive_;
  const storage::DataPageHeader *validated_header_{nullptr};
  bool editable_{false};
  bool dirty_{false};
  PagePayloadProof payload_proof_{PagePayloadProof::None};
};

/*
** TRAVERSAL READ STREAM
**
** Read names the semantic page. A buffered reader uses a pass-through stream
** and accepts no page advice. A direct reader may attach an available native
** engine and stage exact pages, but staged bytes can satisfy only the same
** page ID after validation. Advice failures are not returned; Read falls back
** to its ordinary callback. An active backend copies the page-ID list before
** PrimeExactPages returns.
*/
class PageReadStream final {
 public:
  using ReadFunction = Result<PageHandle> (*)(void *owner, const std::shared_ptr<void> &state, page_id_t page_id);
  using PrimeFunction = void (*)(void *owner, const std::shared_ptr<void> &state,
                                 std::span<const page_id_t> page_ids) noexcept;
  using CloseFunction = void (*)(void *owner, const std::shared_ptr<void> &state) noexcept;

  PageReadStream() = default;
  PageReadStream(void *owner, std::shared_ptr<void> state, ReadFunction read, PrimeFunction prime, CloseFunction close)
      : owner_(owner), state_(std::move(state)), read_(read), prime_(prime), close_(close) {}

  PageReadStream(const PageReadStream &) = delete;
  auto operator=(const PageReadStream &) -> PageReadStream & = delete;
  PageReadStream(PageReadStream &&other) noexcept { Take(std::move(other)); }
  auto operator=(PageReadStream &&other) noexcept -> PageReadStream & {
    if (this != &other) {
      Reset();
      Take(std::move(other));
    }
    return *this;
  }
  ~PageReadStream() { Reset(); }

  auto Read(page_id_t page_id) -> Result<PageHandle>;

  auto AcceptsExactPagePlan() const noexcept -> bool { return state_ != nullptr && prime_ != nullptr; }

  void PrimeExactPages(std::span<const page_id_t> page_ids) noexcept {
    if (AcceptsExactPagePlan() && !page_ids.empty()) {
      prime_(owner_, state_, page_ids);
    }
  }

  // Cancellation discards optional advice state. Read remains usable and
  // invokes read_ without an active plan.
  void CancelAdvice() noexcept {
    if (state_ != nullptr && close_ != nullptr) {
      close_(owner_, state_);
    }
    state_.reset();
    prime_ = nullptr;
    close_ = nullptr;
  }

 private:
  void Reset() noexcept {
    CancelAdvice();
    owner_ = nullptr;
    read_ = nullptr;
  }

  void Take(PageReadStream &&other) noexcept {
    owner_ = std::exchange(other.owner_, nullptr);
    state_ = std::move(other.state_);
    read_ = std::exchange(other.read_, nullptr);
    prime_ = std::exchange(other.prime_, nullptr);
    close_ = std::exchange(other.close_, nullptr);
  }

  void *owner_{nullptr};
  std::shared_ptr<void> state_;
  ReadFunction read_{nullptr};
  PrimeFunction prime_{nullptr};
  CloseFunction close_{nullptr};
};

class PageReader {
 public:
  virtual ~PageReader() = default;

  virtual auto Read(page_id_t page_id) -> Result<PageHandle> = 0;
  virtual auto BeginReadStream() -> PageReadStream;
};

/*
** Mutation contexts add copy/edit, allocation, and retirement. Free declares
** the page logically unreachable; it may be called only with no outstanding
** lease. The concrete source decides when physical ID reuse becomes safe.
*/
class PageSource : public PageReader {
 public:
  virtual auto Edit(page_id_t page_id) -> Result<PageHandle> = 0;
  virtual auto Allocate() -> Result<PageHandle> = 0;
  virtual auto Free(page_id_t page_id) -> Status = 0;
};

}  // namespace tinydb
