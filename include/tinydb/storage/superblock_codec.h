#pragma once

/*
 * The superblock identifies the database format and
 * records the root page of the B+ tree.
 */

#include "tinydb/status.h"
#include "tinydb/storage/page.h"

namespace tinydb::storage {

inline constexpr PageId SUPERBLOCK_PAGE_ID = 0;

struct Superblock {
  PageId root_page_id;
};

[[nodiscard]] auto EncodeSuperblock(const Superblock &superblock)
    -> Result<PageBytes>;
[[nodiscard]] auto DecodeSuperblock(const PageBytes &page)
    -> Result<Superblock>;

} // namespace tinydb::storage
