#pragma once

#include "tinydb/status.h"
#include "tinydb/storage/page_codec.h"
#include <span>
#include <vector>

namespace tinydb::storage {

[[nodiscard]] auto WalRecordSize(std::size_t frame_count)
    -> Result<std::size_t>;
[[nodiscard]] auto EncodeWalRecord(const PageMap &pages)
    -> Result<std::vector<char>>;

[[nodiscard]] auto DecodeWal(std::span<const char> bytes) -> Result<PageMap>;

} // namespace tinydb::storage
