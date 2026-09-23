#pragma once

#include "tinydb/status.h"
#include "tinydb/storage/page_codec.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tinydb::storage {

/*
 * The WAL starts with a header holding a random salt, followed by records
 * stamped with that salt. A reset writes a new salt instead of truncating, so
 * records left from earlier salts end the log.
 */

inline constexpr std::size_t WAL_HEADER_SIZE = 12;

[[nodiscard]] auto EncodeWalHeader(std::uint32_t salt)
    -> std::array<char, WAL_HEADER_SIZE>;
[[nodiscard]] auto WalRecordSize(std::size_t frame_count)
    -> Result<std::size_t>;
[[nodiscard]] auto EncodeWalRecord(const PageMap &pages, std::uint32_t salt)
    -> Result<std::vector<char>>;

[[nodiscard]] auto DecodeWal(std::span<const char> bytes) -> Result<PageMap>;

} // namespace tinydb::storage
