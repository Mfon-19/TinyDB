#pragma once

#include "btree/value.h"

#include <tinydb/status.h>

#include <string>
#include <string_view>
#include <unordered_set>

namespace tinydb {

class PageReader;
class PageSource;

/*
** OVERFLOW VALUE OPERATIONS
**
** PrepareValue chooses inline storage while the resulting record stays below
** the size limit that guarantees a legal leaf split. Otherwise it allocates
** and completely encodes a private overflow chain. A failure leaves partial
** pages private, so the write transaction must abort.
**
** The leaf descriptor stores only the total length and first page ID. Each
** overflow page names its successor, allowing the allocator to use any
** checkpoint-safe page IDs while keeping the leaf descriptor fixed at 16
** bytes. The resulting reads form a dependent page chain: numeric adjacency
** is not authoritative, and a reader may follow only the next link decoded
** from the current authenticated page.
**
** CopyValue and RetireOverflowValue validate the complete chain before
** returning bytes or changing reachability. Integrity validation additionally
** supplies the logical page count and a global ownership set. These values
** prove that each physical overflow page belongs to exactly one leaf value.
*/
auto PrepareValue(PageSource *pages, std::string_view key, std::string_view value) -> Result<LeafValueView>;
auto CopyOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor) -> Result<std::string>;

inline auto CopyValue(PageReader *pages, LeafValueView value) -> Result<std::string> {
  if (!value.IsOverflow()) {
    return std::string(value.InlineBytes());
  }
  return CopyOverflowValue(pages, value.OverflowDescriptor());
}

auto RetireOverflowValue(PageSource *pages, const OverflowValueDescriptor &descriptor) -> Status;

auto ValidateOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor, page_id_t logical_page_count,
                           std::uint64_t maximum_page_lsn, const std::unordered_set<page_id_t> &free_pages,
                           const std::unordered_set<page_id_t> &allocator_pages,
                           std::unordered_set<page_id_t> *claimed_pages) -> Status;

}  // namespace tinydb
