#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <memory>
#include <utility>

namespace tinydb {

// Move-only lease that keeps page bytes stable. The opaque owner and function
// pointer type-erase release without allocating on reads.
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

// Read algorithms depend only on stable immutable leases. Mutation contexts
// extend this boundary instead of making read-only caches expose fake writes.
class PageReader {
 public:
  virtual ~PageReader() = default;

  virtual auto Read(page_id_t page_id) -> Result<PageHandle> = 0;
};

// The mutation vocabulary used by a write overlay or the legacy buffer-pool
// adapter. Free may only retire an ID with no outstanding lease.
class PageSource : public PageReader {
 public:
  virtual auto Edit(page_id_t page_id) -> Result<PageHandle> = 0;
  virtual auto Allocate() -> Result<PageHandle> = 0;
  virtual auto Free(page_id_t page_id) -> Status = 0;
};

}  // namespace tinydb
