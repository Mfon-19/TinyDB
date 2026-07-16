#include "btree/navigation.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "btree/page_view.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace tinydb {

/*
** Descend one immutable page at a time. Every handle is released before the
** next loop iteration. Internal separator equality routes right, matching the
** inclusive-lower-bound invariant used by page builders and verification.
*/
auto FindLeaf(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<page_id_t> {
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
      const auto leaf = LeafPageView::Open(page->Data(), page->Id());
      if (!leaf) {
        return std::unexpected(leaf.error());
      }
      return page_id;
    }
    if (type != static_cast<std::uint16_t>(NodeType::Internal)) {
      return std::unexpected(Status::Corruption("tree descent reached a non-tree page"));
    }
    const auto internal = InternalPageView::Open(page->Data(), page->Id());
    if (!internal) {
      return std::unexpected(internal.error());
    }
    page_id = internal->ChildAt(internal->FindChildIndex(key));
  }
}

}  // namespace tinydb
