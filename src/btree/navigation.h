#pragma once

#include <tinydb/page.h>
#include <tinydb/status.h>

#include <string_view>

namespace tinydb {

class PageSource;

// Constant-space descent for reads. Mutation descent retains a separate path.
auto FindLeaf(PageSource *pages, page_id_t root_page_id, std::string_view key) -> Result<page_id_t>;

}  // namespace tinydb
