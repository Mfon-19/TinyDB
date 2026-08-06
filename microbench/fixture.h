#pragma once

#include "btree/internal_page_builder.h"
#include "btree/leaf_page_builder.h"
#include "storage/page_codec.h"

#include <tinydb/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tinydb::microbench {

template <typename Value>
auto Take(Result<Value> result) -> Value {
  if (!result) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

inline auto Key(std::uint64_t value) -> std::string {
  constexpr auto digits = "0123456789abcdef";
  auto key = std::string(16, '0');
  for (auto index = key.size(); index != 0; --index) {
    key[index - 1U] = digits[static_cast<std::size_t>(value & 0x0FU)];
    value >>= 4U;
  }
  return key;
}

inline auto Bytes(std::size_t size) -> std::vector<std::byte> {
  auto bytes = std::vector<std::byte>(size);
  auto state = std::uint32_t{0x9E3779B9U};
  for (auto &byte : bytes) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    byte = static_cast<std::byte>(state & 0xFFU);
  }
  return bytes;
}

struct LeafFixture {
  std::array<char, PAGE_SIZE> page;
  storage::DataPageHeader header;
  std::vector<std::string> keys;
  std::vector<std::string> missing_keys;
};

inline auto MakeLeafFixture(std::size_t records, page_id_t page_id = FIRST_DATA_PAGE_ID,
                            std::uint64_t page_lsn = 1) -> LeafFixture {
  auto builder = LeafPageBuilder{};
  auto keys = std::vector<std::string>{};
  auto missing_keys = std::vector<std::string>{};
  keys.reserve(records);
  missing_keys.reserve(records);
  for (auto index = std::size_t{0}; index < records; ++index) {
    keys.push_back(Key(2U * index));
    missing_keys.push_back(Key(2U * index + 1U));
    builder.Upsert(keys.back(), LeafValueView::Inline("value-00"));
  }
  if (!builder.Fits()) {
    throw std::runtime_error("microbenchmark leaf fixture does not fit");
  }
  auto page = std::array<char, PAGE_SIZE>{};
  builder.Store(page.data(), page_id);
  auto header = Take(storage::RewriteDataPageLsn(std::as_writable_bytes(std::span{page}), page_id, page_lsn));
  return LeafFixture{
      .page = page,
      .header = header,
      .keys = std::move(keys),
      .missing_keys = std::move(missing_keys),
  };
}

struct InternalFixture {
  std::array<char, PAGE_SIZE> page;
  storage::DataPageHeader header;
  std::vector<std::string> keys;
  std::vector<std::string> missing_keys;
};

inline auto MakeInternalFixture(std::size_t separators, page_id_t page_id = FIRST_DATA_PAGE_ID,
                                std::uint64_t page_lsn = 1) -> InternalFixture {
  if (separators == 0) {
    throw std::runtime_error("microbenchmark internal fixture needs a separator");
  }
  auto keys = std::vector<std::string>{};
  auto missing_keys = std::vector<std::string>{};
  keys.reserve(separators);
  missing_keys.reserve(separators);
  for (auto index = std::size_t{0}; index < separators; ++index) {
    keys.push_back(Key(2U * index));
    missing_keys.push_back(Key(2U * index + 1U));
  }
  auto builder = InternalPageBuilder{FIRST_DATA_PAGE_ID + 1U, keys.front(), FIRST_DATA_PAGE_ID + 2U};
  for (auto index = std::size_t{1}; index < separators; ++index) {
    builder.InsertSeparator(keys[index], FIRST_DATA_PAGE_ID + index + 2U);
  }
  if (!builder.Fits()) {
    throw std::runtime_error("microbenchmark internal fixture does not fit");
  }
  auto page = std::array<char, PAGE_SIZE>{};
  builder.Store(page.data(), page_id);
  auto header = Take(storage::RewriteDataPageLsn(std::as_writable_bytes(std::span{page}), page_id, page_lsn));
  return InternalFixture{
      .page = page,
      .header = header,
      .keys = std::move(keys),
      .missing_keys = std::move(missing_keys),
  };
}

}  // namespace tinydb::microbench
