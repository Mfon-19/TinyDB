#include "cache/committed_page_source.h"

#include "cache/committed_page_cache.h"

#include <expected>
#include <utility>

namespace tinydb::cache {

/* Convert the cache guard without copying bytes or introducing write access. */
auto CommittedPageSource::Read(page_id_t page_id) -> Result<PageHandle> {
  auto page = cache_->Read(page_id);
  if (!page) {
    return std::unexpected(std::move(page).error());
  }
  return std::move(*page).IntoPageHandle();
}

}  // namespace tinydb::cache
