#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <memory>
#include <utility>

namespace tinydb {

/*
** PAGE ACCESS BOUNDARY
**
** B+ tree algorithms know only four operations: Read, Edit, Allocate and
** Free. They do not know whether bytes came from the committed cache, a write
** transaction overlay, or an in-memory test model.
**
** Every successful operation returns a move-only PageHandle. While the handle
** lives, its byte address and page identity are stable. A source may not reuse
** or destroy the underlying page until the handle invokes its release
** callback. Read handles are immutable. Edit and Allocate handles accumulate
** a sticky dirty bit that is returned to their source exactly once.
**
** The opaque owner and function pointer type-erase release without allocating
** a wrapper on the ordinary read path.
*/
class PageHandle {
 public:
  using Release = void (*)(void *owner, page_id_t page_id, bool dirty);

  PageHandle() = default;
  PageHandle(void *owner, page_id_t page_id, char *data, bool editable, Release release)
      : owner_(owner), page_id_(page_id), data_(data), mutable_data_(data), editable_(editable), release_(release) {}

  // Immutable caches retain a frame through keepalive while release updates
  // frame-local pin accounting. No per-read wrapper allocation is required.
  PageHandle(void *owner, page_id_t page_id, const char *data, Release release, std::shared_ptr<const void> keepalive)
      : owner_(owner), page_id_(page_id), data_(data), release_(release), keepalive_(std::move(keepalive)) {}

  PageHandle(const PageHandle &) = delete;
  auto operator=(const PageHandle &) -> PageHandle & = delete;

  PageHandle(PageHandle &&other) noexcept { Take(std::move(other)); }

  auto operator=(PageHandle &&other) noexcept -> PageHandle & {
    if (this != &other) {
      Reset();
      Take(std::move(other));
    }
    return *this;
  }

  ~PageHandle() { Reset(); }

  auto Id() const -> page_id_t { return page_id_; }
  auto Data() const -> const char * { return data_; }

  // Only Edit and Allocate may produce an editable handle.
  auto MutableData() -> char *;
  void MarkDirty();

 private:
  void Reset() noexcept;
  void Take(PageHandle &&other) noexcept;

  void *owner_{nullptr};
  page_id_t page_id_{HEADER_PAGE_ID};
  const char *data_{nullptr};
  char *mutable_data_{nullptr};
  bool editable_{false};
  bool dirty_{false};
  Release release_{nullptr};
  std::shared_ptr<const void> keepalive_;
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
