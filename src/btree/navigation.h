#pragma once

#include "btree/page_source.h"

#include <tinydb/status.h>
#include "storage/page.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace tinydb {

class PageReader;

/*
** Return the pinned leaf whose routing range contains key. Read descent keeps
** no ancestor handles or path allocation, and the caller reuses the final
** lease instead of looking the leaf up a second time.
*/
auto FindLeaf(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<PageHandle>;

/* Return the leftmost leaf without inventing a sentinel key. */
auto FindFirstLeaf(PageReader *pages, page_id_t root_page_id) -> Result<PageHandle>;

/*
** Return a bounded list of exact leaf successors. The planner reads internal
** edges only. An empty result leaves the later leaf-chain traversal unchanged.
*/
auto PlanLeafPageSuccessorsForReadAhead(
    PageReader *pages, page_id_t root_page_id, page_id_t current_leaf_page_id, page_id_t logical_page_count,
    std::string_view current_key, std::size_t page_count,
    std::optional<std::string_view> upper = std::nullopt) noexcept -> std::vector<page_id_t>;

}  // namespace tinydb
