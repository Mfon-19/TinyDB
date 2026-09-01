#pragma once

/*
 * The superblock identifies the database format and 
 * records the root page of the B+ tree.
 */

#include "tinydb/status.h"
#include "tinydb/storage/page.h"

namespace tinydb::storage {

struct Superblock {
  PageId root_page_id;
};

auto EncodeSuperblock(const Superblock &superblock) -> Result<PageBytes>;
auto DecodeSuperblock(const PageBytes &page) -> Result<Superblock>;

} // namespace tinydb::storage
