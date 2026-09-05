#pragma once

/*
 * Leaf and internal pages use a slotted page format. The slot directory
 * grows from the header while variable-sized cells grow backward
 * from the end of the page.
 */

#include "tinydb/status.h"
#include "tinydb/storage/page.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tinydb::storage {

enum class PageType : std::uint16_t { Leaf = 1, Internal = 2 };

struct LeafEntry {
  std::string_view key;
  std::string_view value;
};

struct InternalEntry {
  std::string_view key;
  PageId right_child;
};

class LeafPageView {
public:
  [[nodiscard]] auto Id() const noexcept -> PageId { return page_id_; }
  [[nodiscard]] auto NextLeaf() const noexcept -> PageId { return next_leaf_; }
  [[nodiscard]] auto EntryCount() const noexcept -> std::size_t {
    return entry_count_;
  }
  [[nodiscard]] auto Entry(std::size_t index) const noexcept -> LeafEntry;

private:
  friend class Page;

  LeafPageView(const PageBytes *page, PageId page_id, PageId next_leaf,
               std::uint16_t entry_count) noexcept
      : page_(page), page_id_(page_id), next_leaf_(next_leaf),
        entry_count_(entry_count) {}

  const PageBytes *page_;
  PageId page_id_;
  PageId next_leaf_;
  std::uint16_t entry_count_;
};

class InternalPageView {
public:
  [[nodiscard]] auto Id() const noexcept -> PageId { return page_id_; }
  [[nodiscard]] auto LeftmostChild() const noexcept -> PageId {
    return leftmost_child_;
  }
  [[nodiscard]] auto EntryCount() const noexcept -> std::size_t {
    return entry_count_;
  }
  [[nodiscard]] auto Entry(std::size_t index) const noexcept -> InternalEntry;

private:
  friend class Page;

  InternalPageView(const PageBytes *page, PageId page_id, PageId leftmost_child,
                   std::uint16_t entry_count) noexcept
      : page_(page), page_id_(page_id), leftmost_child_(leftmost_child),
        entry_count_(entry_count) {}

  const PageBytes *page_;
  PageId page_id_;
  PageId leftmost_child_;
  std::uint16_t entry_count_;
};

// Owns validated bytes. Views borrow them until the page is replaced.
class Page {
public:
  [[nodiscard]] auto Bytes() const noexcept -> const PageBytes & {
    return bytes_;
  }
  [[nodiscard]] auto Id() const noexcept -> PageId;
  [[nodiscard]] auto Type() const noexcept -> PageType;
  [[nodiscard]] auto FreeSpace() const noexcept -> std::size_t;
  [[nodiscard]] auto PayloadSize() const noexcept -> std::size_t;
  [[nodiscard]] auto Leaf() const noexcept -> LeafPageView;
  [[nodiscard]] auto Internal() const noexcept -> InternalPageView;

private:
  friend auto DecodePage(PageId expected_page_id, const PageBytes &bytes)
      -> Result<Page>;
  explicit Page(const PageBytes &bytes) noexcept : bytes_(bytes) {}

  PageBytes bytes_;
};

auto DecodePage(PageId expected_page_id, const PageBytes &bytes) -> Result<Page>;

auto EncodeLeafPage(PageId page_id, PageId next_leaf,
                    std::span<const LeafEntry> entries) -> Result<PageBytes>;

auto EncodeInternalPage(PageId page_id, PageId leftmost_child,
                        std::span<const InternalEntry> entries)
    -> Result<PageBytes>;

} // namespace tinydb::storage
