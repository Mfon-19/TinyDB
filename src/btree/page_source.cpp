#include "btree/page_source.h"

#include <expected>

namespace tinydb {

auto PageReadStream::Read(page_id_t page_id) -> Result<PageHandle> {
  if (read_ == nullptr) {
    return std::unexpected(Status::Closed("reading through an empty page stream"));
  }
  return read_(owner_, state_, page_id);
}

auto PageReader::BeginReadStream() -> PageReadStream {
  // Readers without an advice backend use the same stream interface. The
  // null state disables planning and forwards each semantic read unchanged.
  const auto read = [](void *owner, const std::shared_ptr<void> &, page_id_t page_id) -> Result<PageHandle> {
    return static_cast<PageReader *>(owner)->Read(page_id);
  };
  return {this, {}, read, nullptr, nullptr};
}

}  // namespace tinydb
