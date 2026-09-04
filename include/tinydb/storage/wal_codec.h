#pragma once

#include "tinydb/status.h"
#include "tinydb/storage/page.h"
#include <map>
#include <span>
#include <vector>

namespace tinydb::storage {

using WalPages = std::map<PageId, PageBytes>;

auto WalRecordSize(std::size_t frame_count) -> Result<std::size_t>;
auto EncodeWalRecord(const WalPages &pages) -> Result<std::vector<char>>;
auto DecodeWal(std::span<const char> bytes) -> Result<WalPages>;

} // namespace tinydb::storage
