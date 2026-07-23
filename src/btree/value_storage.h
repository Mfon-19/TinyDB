#pragma once

#include "btree/value.h"

#include <tinydb/status.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace tinydb {

class PageReader;
class PageSource;

/*
** OVERFLOW VALUE OPERATIONS
**
** PrepareValue chooses inline storage when one record remains below the
** byte-split safety bound. Otherwise it allocates and completely encodes a
** private chain. A failure leaves partial pages private; the write transaction
** must abort, just like a tree split that runs out of memory.
**
** CopyValue and RetireOverflowValue validate the complete chain before
** returning bytes or changing reachability. Integrity validation additionally
** supplies the allocation frontier and a global ownership set, proving that
** every physical overflow page belongs to exactly one leaf value.
*/
auto PrepareValue(PageSource *pages, std::string_view key, std::string_view value) -> Result<LeafValue>;
auto CopyOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor) -> Result<std::string>;

inline auto CopyValue(PageReader *pages, LeafValueView value) -> Result<std::string> {
  if (!value.IsOverflow()) {
    return std::string(value.InlineBytes());
  }
  return CopyOverflowValue(pages, value.OverflowDescriptor());
}

auto RetireOverflowValue(PageSource *pages, const OverflowValueDescriptor &descriptor) -> Status;

auto ValidateOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor, page_id_t high_water_page_id,
                           std::uint64_t maximum_page_lsn, const std::unordered_set<page_id_t> &free_pages,
                           const std::unordered_set<page_id_t> &allocator_pages,
                           std::unordered_set<page_id_t> *claimed_pages) -> Status;

}  // namespace tinydb
