#pragma once

/*
 * A PageHandle is a RAII guard that pins a frame for as long 
 * as the handle exists.
 */

#include "tinydb/storage/page.h"
#include <cassert>
#include <cstddef>
#include <utility>

namespace tinydb::cache {

class BufferPool;

class PageHandle {
public:
  PageHandle(const PageHandle &) = delete;
  PageHandle &operator=(const PageHandle &) = delete;

  PageHandle(PageHandle &&other) noexcept
      : page_(std::exchange(other.page_, nullptr)),
        pin_count_(std::exchange(other.pin_count_, nullptr)) {}

  PageHandle &operator=(PageHandle &&other) noexcept {
    if (this != &other) {
      Release();
      page_ = std::exchange(other.page_, nullptr);
      pin_count_ = std::exchange(other.pin_count_, nullptr);
    }
    return *this;
  }

  ~PageHandle() { Release(); }

  [[nodiscard]] auto Bytes() const noexcept -> const storage::PageBytes & {
    return *page_;
  }

private:
  friend class BufferPool;

  PageHandle(const storage::PageBytes *page, std::size_t *pin_count) noexcept
      : page_(page), pin_count_(pin_count) {
    ++*pin_count_;
  }

  void Release() noexcept {
    if (pin_count_ != nullptr) {
      assert(*pin_count_ > 0);
      --*pin_count_;
      page_ = nullptr;
      pin_count_ = nullptr;
    }
  }

  const storage::PageBytes *page_;
  std::size_t *pin_count_;
};

} // namespace tinydb::cache
