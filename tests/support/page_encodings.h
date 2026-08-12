#pragma once

#include "storage/page_codec.h"
#include "storage/page.h"

#include <array>
#include <expected>
#include <span>
#include <utility>

namespace tinydb::test {

/*
** One-shot checksummed page encoders for test fixtures. Production writers
** emit canonical private bytes through the Initialize functions and defer the
** checksum to commit sealing, so these finalizing wrappers live with the
** tests that persist standalone page images.
*/
inline auto EncodeFreeExtentPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t next_page_id,
                                 std::span<const storage::FreeExtent> extents)
    -> Result<std::array<char, PAGE_SIZE>> {
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status = storage::InitializeFreeExtentPage(bytes, page_id, page_lsn, next_page_id, extents);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  storage::FinalizeDataPage(bytes);
  return output;
}

inline auto EncodeOverflowPage(page_id_t page_id, std::uint64_t page_lsn, page_id_t owner_value_id,
                               std::uint32_t chunk_index, page_id_t next_page_id,
                               std::span<const std::byte> payload) -> Result<std::array<char, PAGE_SIZE>> {
  auto output = std::array<char, PAGE_SIZE>{};
  auto bytes = std::as_writable_bytes(std::span{output});
  if (auto status =
          storage::InitializeOverflowPage(bytes, page_id, page_lsn, owner_value_id, chunk_index, next_page_id, payload);
      !status.Ok()) {
    return std::unexpected(std::move(status));
  }
  storage::FinalizeDataPage(bytes);
  return output;
}

}  // namespace tinydb::test
