#pragma once

#include "btree/page_source.h"

#include <tinydb/status.h>
#include "storage/page.h"

#include <string_view>

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

}  // namespace tinydb
