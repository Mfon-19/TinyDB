#include "btree/value_storage.h"

#include "btree/page_format.h"
#include "btree/page_source.h"
#include "storage/page_codec.h"
#include "util/crc32.h"

#include <tinydb/bytes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tinydb {
namespace {

struct ChainConstraints {
  page_id_t high_water_page_id{std::numeric_limits<page_id_t>::max()};
  std::uint64_t maximum_page_lsn{std::numeric_limits<std::uint64_t>::max()};
  const std::unordered_set<page_id_t> *free_pages{nullptr};
  const std::unordered_set<page_id_t> *allocator_pages{nullptr};
  std::unordered_set<page_id_t> *claimed_pages{nullptr};
};

/*
** Walk one descriptor without trusting any link or redundant field. The
** visitor runs only after the current page's identity, owner, chunk index, and
** canonical payload length have been checked. The final logical checksum is
** compared only after the terminator proves the exact advertised length.
*/
template <typename Visitor>
auto WalkOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor,
                       const ChainConstraints &constraints, Visitor visit) -> Status {
  TINYDB_CHECK(pages != nullptr, "overflow traversal requires a page reader");
  if (descriptor.total_value_bytes == 0 || descriptor.total_value_bytes > MAX_VALUE_BYTES ||
      descriptor.first_page_id < FIRST_DATA_PAGE_ID) {
    return Status::Corruption("invalid overflow value descriptor");
  }

  auto checksum = Crc32Accumulator{};
  auto page_id = descriptor.first_page_id;
  auto remaining = descriptor.total_value_bytes;
  auto expected_chunk = std::uint32_t{0};

  for (;;) {
    if (page_id < FIRST_DATA_PAGE_ID || page_id >= constraints.high_water_page_id) {
      return Status::Corruption("overflow page lies outside the allocation frontier");
    }
    if ((constraints.free_pages != nullptr && constraints.free_pages->contains(page_id)) ||
        (constraints.allocator_pages != nullptr && constraints.allocator_pages->contains(page_id))) {
      return Status::Corruption("overflow chain references non-value storage");
    }
    if (constraints.claimed_pages != nullptr && !constraints.claimed_pages->insert(page_id).second) {
      return Status::Corruption("overflow page is owned by more than one reachable object");
    }

    auto page = pages->Read(page_id);
    if (!page) {
      return std::move(page).error();
    }
    const auto bytes = std::as_bytes(std::span{page->Data(), PAGE_SIZE});
    const auto *const validated_header = page->ValidatedHeader();
    const auto decoded = validated_header != nullptr ? storage::DecodeOverflowPage(bytes, page->Id(), *validated_header)
                                                     : storage::DecodeOverflowPage(bytes, page->Id());
    if (!decoded) {
      return decoded.error();
    }
    if (decoded->page_lsn > constraints.maximum_page_lsn) {
      return Status::Corruption("overflow page LSN is newer than the verified snapshot");
    }
    if (decoded->owner_value_id != descriptor.first_page_id || decoded->chunk_index != expected_chunk) {
      return Status::Corruption("overflow page has the wrong owner or chunk index");
    }
    if (decoded->payload.size() > remaining) {
      return Status::Corruption("overflow chain exceeds its advertised value length");
    }

    const auto final_page = decoded->next_page_id == HEADER_PAGE_ID;
    if (final_page != (decoded->payload.size() == remaining) ||
        (!final_page && decoded->payload.size() != storage::OVERFLOW_PAGE_PAYLOAD_BYTES)) {
      return Status::Corruption("overflow chain is truncated or non-canonical");
    }

    checksum.Update(decoded->payload);
    if (auto status = visit(page_id, decoded->payload); !status.Ok()) {
      return status;
    }
    remaining -= decoded->payload.size();
    if (final_page) {
      break;
    }
    if (expected_chunk == std::numeric_limits<std::uint32_t>::max()) {
      return Status::Corruption("overflow chunk index exceeds its encoded range");
    }
    ++expected_chunk;
    page_id = decoded->next_page_id;
  }

  return checksum.Finish() == descriptor.value_checksum
             ? Status{}
             : Status::Corruption("overflow value checksum does not match its descriptor");
}

}  // namespace

/*
** Return an owning leaf representation for value. All overflow pages are
** allocated, encoded, and marked dirty before success. Any error means the
** caller must abort its private page transaction; no committed state changed.
*/
auto PrepareValue(PageSource *pages, std::string_view key, std::string_view value) -> Result<LeafValueView> {
  TINYDB_CHECK(pages != nullptr, "overflow preparation requires a page source");
  if (value.size() > MAX_VALUE_BYTES) {
    return std::unexpected(Status::InvalidArgument("value exceeds the maximum value size"));
  }
  const auto inline_footprint = SLOT_SIZE + LEAF_CELL_HEADER_SIZE + key.size() + value.size();
  if (inline_footprint <= MAX_LEAF_RECORD_BYTES) {
    // The builder copies this borrowed value before Put returns.
    return LeafValueView::Inline(value);
  }

  const auto chunks = (value.size() + storage::OVERFLOW_PAGE_PAYLOAD_BYTES - 1) / storage::OVERFLOW_PAGE_PAYLOAD_BYTES;
  static_assert(MAX_VALUE_BYTES / storage::OVERFLOW_PAGE_PAYLOAD_BYTES < std::numeric_limits<std::uint32_t>::max());
  auto allocated = std::vector<PageHandle>{};
  allocated.reserve(chunks);
  for (std::size_t index = 0; index < chunks; ++index) {
    auto page = pages->Allocate();
    if (!page) {
      return std::unexpected(std::move(page).error());
    }
    allocated.push_back(std::move(*page));
  }

  const auto value_bytes = std::as_bytes(std::span{value.data(), value.size()});
  const auto owner_value_id = allocated.front().Id();
  for (std::size_t index = 0; index < allocated.size(); ++index) {
    const auto offset = index * storage::OVERFLOW_PAGE_PAYLOAD_BYTES;
    const auto count = std::min(storage::OVERFLOW_PAGE_PAYLOAD_BYTES, value.size() - offset);
    const auto next = index + 1 < allocated.size() ? allocated[index + 1].Id() : HEADER_PAGE_ID;
    auto page = std::as_writable_bytes(std::span<char, PAGE_SIZE>{allocated[index].MutableData(), PAGE_SIZE});
    if (auto status = storage::InitializeOverflowPage(page, allocated[index].Id(), 0, owner_value_id,
                                                      static_cast<std::uint32_t>(index), next,
                                                      value_bytes.subspan(offset, count));
        !status.Ok()) {
      return std::unexpected(std::move(status));
    }
    allocated[index].MarkDirty();
  }

  return LeafValueView::Overflow(OverflowValueDescriptor{
      .total_value_bytes = value.size(),
      .first_page_id = owner_value_id,
      .value_checksum = Crc32(value_bytes),
  });
}

/* Copy and authenticate one overflow value. No borrowed page bytes escape. */
auto CopyOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor) -> Result<std::string> {
  auto output = std::string{};
  output.reserve(static_cast<std::size_t>(descriptor.total_value_bytes));
  auto status = WalkOverflowValue(pages, descriptor, ChainConstraints{},
                                  [&output](page_id_t, std::span<const std::byte> payload) {
                                    output.append(reinterpret_cast<const char *>(payload.data()), payload.size());
                                    return Status{};
                                  });
  if (!status.Ok()) {
    return std::unexpected(status);
  }
  return output;
}

/*
** Prove the complete old chain first, then retire every page through the
** transaction allocator. Failure is transaction-fatal but never mutates the
** committed allocator.
*/
auto RetireOverflowValue(PageSource *pages, const OverflowValueDescriptor &descriptor) -> Status {
  // Validate before the first Free so a malformed chain never produces a
  // partially retired transaction state. The caller still aborts on any later
  // allocator failure.
  auto page_ids = std::vector<page_id_t>{};
  auto status = WalkOverflowValue(pages, descriptor, ChainConstraints{},
                                  [&page_ids](page_id_t page_id, std::span<const std::byte>) {
                                    page_ids.push_back(page_id);
                                    return Status{};
                                  });
  if (!status.Ok()) {
    return status;
  }
  for (const auto page_id : page_ids) {
    if (auto free_status = pages->Free(page_id); !free_status.Ok()) {
      return free_status;
    }
  }
  return {};
}

/* Join chain-local validation to the verifier's global page-ownership proof. */
auto ValidateOverflowValue(PageReader *pages, const OverflowValueDescriptor &descriptor, page_id_t high_water_page_id,
                           std::uint64_t maximum_page_lsn, const std::unordered_set<page_id_t> &free_pages,
                           const std::unordered_set<page_id_t> &allocator_pages,
                           std::unordered_set<page_id_t> *claimed_pages) -> Status {
  return WalkOverflowValue(pages, descriptor,
                           ChainConstraints{
                               .high_water_page_id = high_water_page_id,
                               .maximum_page_lsn = maximum_page_lsn,
                               .free_pages = &free_pages,
                               .allocator_pages = &allocator_pages,
                               .claimed_pages = claimed_pages,
                           },
                           [](page_id_t, std::span<const std::byte>) { return Status{}; });
}

}  // namespace tinydb
