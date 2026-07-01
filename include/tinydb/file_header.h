#pragma once

#include <tinydb/page.h>
#include <cstdint>

namespace tinydb {

// FileHeader is the logical contents of page 0.
//
// The current storage layer writes this struct at the start of page 0. Keep
// this small and plain until the file format needs portable encoding.
struct FileHeader {
  std::uint32_t magic;
  std::uint32_t page_size;

  // Page 0 is reserved for this header, so 0 also represents "no root yet".
  page_id_t root_page_id;

  // The next page id to allocate. New database files start at page 1.
  page_id_t next_page_id;
};
}  // namespace tinydb
