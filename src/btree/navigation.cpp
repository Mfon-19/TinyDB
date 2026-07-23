#include "btree/navigation.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "btree/page_view.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace tinydb {
namespace {

/*
** Descend one immutable page at a time. choose_child receives a validated
** internal view and selects the next edge. Every handle is released before
** the next iteration. The depth bound detects parent cycles without retaining
** an allocation-sized visited set.
*/
template <typename ChooseChild>
auto Descend(PageReader *pages, page_id_t root_page_id, ChooseChild choose_child) -> Result<PageHandle> {
  auto page_id = root_page_id;
  auto depth = std::size_t{0};
  for (;;) {
    // Sixty-four levels exceed the representable page population at minimum
    // fanout, so this constant-space bound also detects parent cycles.
    if (++depth > 64) {
      return std::unexpected(Status::Corruption("tree descent is too deep or cyclic"));
    }
    auto page = pages->Read(page_id);
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    const auto type = RawNodeType(page->Data());
    if (type == static_cast<std::uint16_t>(NodeType::Leaf)) {
      // The consumer opens and validates this leaf while retaining the same
      // lease. Returning it avoids a second cache lookup and pin cycle.
      return std::move(*page);
    }
    if (type != static_cast<std::uint16_t>(NodeType::Internal)) {
      return std::unexpected(Status::Corruption("tree descent reached a non-tree page"));
    }
    const auto internal = InternalPageView::Open(*page);
    if (!internal) {
      return std::unexpected(internal.error());
    }
    page_id = choose_child(*internal);
  }
}

}  // namespace

auto FindLeaf(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<PageHandle> {
  // Separator equality routes right, matching the inclusive-lower-bound
  // invariant used by page builders and verification.
  return Descend(pages, root_page_id,
                 [key](const InternalPageView &page) { return page.ChildAt(page.FindChildIndex(key)); });
}

auto FindFirstLeaf(PageReader *pages, page_id_t root_page_id) -> Result<PageHandle> {
  return Descend(pages, root_page_id, [](const InternalPageView &page) { return page.ChildAt(0); });
}

}  // namespace tinydb
