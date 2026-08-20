#pragma once

#include <array>
#include <cstddef>

namespace tinydb {

/*
** A DatabaseUuid identifies one database and appears in both superblocks as
** well as the WAL header.  Recovery rejects a WAL whose UUID does not match
** the superblock, so a WAL copied from another database cannot be replayed
** against this one.
*/
using DatabaseUuid = std::array<std::byte, 16>;

}  // namespace tinydb
