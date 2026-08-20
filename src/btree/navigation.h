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
** Return the pinned leaf whose routing range contains key. Descent retains no
** ancestor handles or path allocation; the final lease is returned so the
** caller does not perform a second cache lookup and pin operation.
*/
auto FindLeaf(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<PageHandle>;

auto FindFirstLeaf(PageReader *pages, page_id_t root_page_id) -> Result<PageHandle>;

/*
** Return at most page_count leaves that follow current_leaf_page_id in key
** order, using only authenticated internal-tree edges. The direct-I/O cursor
** uses the result as optional read-ahead advice; buffered cursors do not call
** this function. An empty result means that traversal must continue from the
** leaf chain without advice.
*/
auto PlanLeafPageSuccessorsForReadAhead(
    PageReader *pages, page_id_t root_page_id, page_id_t current_leaf_page_id, page_id_t logical_page_count,
    std::string_view current_key, std::size_t page_count,
    std::optional<std::string_view> upper = std::nullopt) noexcept -> std::vector<page_id_t>;

}  // namespace tinydb
