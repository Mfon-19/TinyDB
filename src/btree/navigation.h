#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <string_view>

namespace tinydb {

class PageReader;

/*
** Return the leaf whose routing range contains key. Read descent keeps no
** ancestor handles or path allocation; mutation descent separately retains
** page IDs needed for split propagation and occupancy repair.
*/
auto FindLeaf(PageReader *pages, page_id_t root_page_id, std::string_view key) -> Result<page_id_t>;

}  // namespace tinydb
