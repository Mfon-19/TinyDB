#pragma once

#include <tinydb/status.h>
#include "storage/page.h"
#include "util/check.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace tinydb {

namespace storage {
struct DataPageHeader;
}

// Transaction-private pages can defer their checksum until commit when an
// internal builder or codec has proved the complete type-specific payload.
// Any later mutable access clears this proof.
enum class PagePayloadProof : std::uint8_t {
  None,
  Tree,
  Overflow,
};

/*
** PAGE ACCESS BOUNDARY
**
** B+ tree algorithms know only four operations: Read, Edit, Allocate and
** Free. They do not know whether bytes came from the committed cache, a write
** transaction overlay, or an in-memory test model.
**
** Every successful operation returns a move-only PageHandle. While the handle
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

  // Immutable caches retain a frame through keepalive. Its shared ownership is
  // also the exact eviction pin, so release needs no owner callback.
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

  // Only Edit and Allocate may produce an editable handle.
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

/* Read algorithms depend only on stable immutable leases. */
class PageReader {
 public:
  virtual ~PageReader() = default;

  virtual auto Read(page_id_t page_id) -> Result<PageHandle> = 0;
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
