#pragma once

#include <tinydb/page.h>

#include <cstdint>

namespace tinydb {

// "TDB2". Doubles as the format version: a header-layout change mints a new
// magic, and files carrying an old one are rejected as foreign.
constexpr std::uint32_t TINYDB_FILE_MAGIC = 0x54444232U;

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

  // Head of the free-page list, threaded through the free pages themselves
  // (each stores a FreePageHeader). HEADER_PAGE_ID means the list is empty.
  page_id_t free_list_head;

  // CRC-32 of every field above (computed by HeaderChecksum in
  // src/util/checksums.h). This is what lets Open and recovery tell a torn
  // header write — our magic with a bad checksum, repairable from a logged
  // header image — from a foreign file that must not be touched.
  std::uint32_t checksum;
};

// Marks a page on the free list. The value is distinct from the B+ tree node
// types (see src/btree/page_format.h), so any dangling reference into a
// freed page aborts loudly the moment a node codec tries to load it.
constexpr std::uint16_t FREE_PAGE_TYPE = 3;

// The first bytes of a page on the free list; the rest of the page is dead.
struct FreePageHeader {
  std::uint16_t type;   // always FREE_PAGE_TYPE
  page_id_t next_free;  // HEADER_PAGE_ID terminates the list
};

// These structs are on-disk formats: a size change (new member, reordered
// members, different padding) is a file-format change and must be deliberate.
static_assert(sizeof(FileHeader) == 40);
static_assert(sizeof(FreePageHeader) == 16);
}  // namespace tinydb
