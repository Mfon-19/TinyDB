#include "tinydb/storage/page_codec.h"
#include "tinydb/storage/crc32.h"
#include "tinydb/storage/encoding.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

/*
 * Page layout:
 *
 *   header          magic, version, type, page ID, checksum, entry
 *                   count, link
 *   slot directory  a u16 cell offset per entry ordered by key
 *   cells           packed back to back at the end of the page
 */

namespace tinydb::storage {

namespace {

inline constexpr std::array<char, 4> PAGE_MAGIC = {'T', 'D', 'P', '1'};
inline constexpr std::uint16_t FORMAT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t VERSION_OFFSET = 4;
inline constexpr std::size_t TYPE_OFFSET = 6;
inline constexpr std::size_t PAGE_ID_OFFSET = 8;
inline constexpr std::size_t CHECKSUM_OFFSET = 12;
inline constexpr std::size_t ENTRY_COUNT_OFFSET = 16;
inline constexpr std::size_t LINK_OFFSET = 18;
inline constexpr std::size_t HEADER_SIZE = 22;
inline constexpr std::size_t SLOT_SIZE = sizeof(std::uint16_t);
inline constexpr std::size_t KEY_SIZE_SIZE = sizeof(std::uint16_t);

auto ValidDataPageId(PageId page_id) noexcept -> bool {
  return page_id != 0 && page_id != INVALID_PAGE_ID;
}

// A link may only lead to a data page other than the one holding it.
auto ValidLink(PageId page_id, PageId link) noexcept -> bool {
  return ValidDataPageId(link) && link != page_id;
}

auto ValueSize(const LeafEntry &entry) noexcept -> std::size_t {
  return entry.value.size();
}

auto ValueSize(const InternalEntry & /*entry*/) noexcept -> std::size_t {
  return sizeof(PageId);
}

void PutValue(PageBytes &page, std::size_t offset, const LeafEntry &entry) {
  std::ranges::copy(entry.value, page.begin() + offset);
}

void PutValue(PageBytes &page, std::size_t offset, const InternalEntry &entry) {
  little_endian::PutU32(page, offset, entry.right_child);
}

auto SlotOffset(const PageBytes &page,
                std::size_t index) noexcept -> std::size_t {
  return little_endian::GetU16(page, HEADER_SIZE + index * SLOT_SIZE);
}

auto CellBounds(const PageBytes &page, std::size_t entry_count,
                std::size_t index) noexcept
    -> std::pair<std::size_t, std::size_t> {
  const std::size_t end =
      index + 1 < entry_count ? SlotOffset(page, index + 1) : PAGE_SIZE;
  return {SlotOffset(page, index), end};
}

auto CellAt(const PageBytes &page, std::size_t entry_count,
            std::size_t index) noexcept -> LeafEntry {
  const auto [begin, end] = CellBounds(page, entry_count, index);
  const std::size_t key_begin = begin + KEY_SIZE_SIZE;
  const std::size_t value_begin =
      key_begin + little_endian::GetU16(page, begin);
  return {{page.data() + key_begin, value_begin - key_begin},
          {page.data() + value_begin, end - value_begin}};
}

void EncodeHeader(PageBytes &page, PageType type, PageId page_id,
                  std::uint16_t entry_count, PageId link) noexcept {
  std::ranges::copy(PAGE_MAGIC, page.begin() + MAGIC_OFFSET);
  little_endian::PutU16(page, VERSION_OFFSET, FORMAT_VERSION);
  little_endian::PutU16(page, TYPE_OFFSET, static_cast<std::uint16_t>(type));
  little_endian::PutU32(page, PAGE_ID_OFFSET, page_id);
  little_endian::PutU16(page, ENTRY_COUNT_OFFSET, entry_count);
  little_endian::PutU32(page, LINK_OFFSET, link);
  little_endian::PutU32(
      page, CHECKSUM_OFFSET,
      Crc32WithZeroedU32(std::span<const char>{page}, CHECKSUM_OFFSET));
}

template <typename Entry>
auto EncodePage(PageType type, PageId page_id, PageId link,
                std::span<const Entry> entries) -> Result<PageBytes> {
  std::size_t size = HEADER_SIZE;
  for (const auto &entry : entries) {
    size += SLOT_SIZE + KEY_SIZE_SIZE + entry.key.size() + ValueSize(entry);
  }
  if (size > PAGE_SIZE) {
    return Err(Status::InvalidArgument("entries do not fit in a page"));
  }

  PageBytes page{};
  std::size_t cell = PAGE_SIZE;
  for (std::size_t index = entries.size(); index != 0; --index) {
    const auto &entry = entries[index - 1];
    cell -= KEY_SIZE_SIZE + entry.key.size() + ValueSize(entry);
    little_endian::PutU16(page, HEADER_SIZE + (index - 1) * SLOT_SIZE,
                          static_cast<std::uint16_t>(cell));
    little_endian::PutU16(page, cell,
                          static_cast<std::uint16_t>(entry.key.size()));
    std::ranges::copy(entry.key, page.begin() + cell + KEY_SIZE_SIZE);
    PutValue(page, cell + KEY_SIZE_SIZE + entry.key.size(), entry);
  }

  EncodeHeader(page, type, page_id, static_cast<std::uint16_t>(entries.size()),
               link);
  return page;
}

} // namespace

auto LeafPageView::Entry(std::size_t index) const noexcept -> LeafEntry {
  assert(index < entry_count_);
  return CellAt(*page_, entry_count_, index);
}

auto InternalPageView::Entry(std::size_t index) const noexcept
    -> InternalEntry {
  assert(index < entry_count_);
  const auto cell = CellAt(*page_, entry_count_, index);
  return {cell.key, little_endian::GetU32(cell.value, 0)};
}

auto Page::Id() const noexcept -> PageId {
  return little_endian::GetU32(bytes_, PAGE_ID_OFFSET);
}

auto Page::Type() const noexcept -> PageType {
  return static_cast<PageType>(little_endian::GetU16(bytes_, TYPE_OFFSET));
}

auto Page::Leaf() const noexcept -> LeafPageView {
  assert(Type() == PageType::Leaf);
  return LeafPageView{&bytes_, Id(), little_endian::GetU32(bytes_, LINK_OFFSET),
                      little_endian::GetU16(bytes_, ENTRY_COUNT_OFFSET)};
}

auto Page::Internal() const noexcept -> InternalPageView {
  assert(Type() == PageType::Internal);
  return InternalPageView{&bytes_, Id(),
                          little_endian::GetU32(bytes_, LINK_OFFSET),
                          little_endian::GetU16(bytes_, ENTRY_COUNT_OFFSET)};
}

auto DecodePage(PageId expected_page_id, const PageBytes &page) -> Result<Page> {
  if (!ValidDataPageId(expected_page_id)) {
    return Err(Status::InvalidArgument("invalid data page ID"));
  }

  const std::span<const char> bytes{page};
  if (!std::ranges::equal(PAGE_MAGIC,
                          bytes.subspan(MAGIC_OFFSET, PAGE_MAGIC.size()))) {
    return Err(Status::Corruption("invalid page magic"));
  }
  if (little_endian::GetU16(bytes, VERSION_OFFSET) != FORMAT_VERSION) {
    return Err(Status::Corruption("unsupported page version"));
  }
  const auto type =
      static_cast<PageType>(little_endian::GetU16(bytes, TYPE_OFFSET));
  if (type != PageType::Leaf && type != PageType::Internal) {
    return Err(Status::Corruption("unexpected page type"));
  }
  if (little_endian::GetU32(bytes, PAGE_ID_OFFSET) != expected_page_id) {
    return Err(Status::Corruption("page ID mismatch"));
  }
  if (little_endian::GetU32(bytes, CHECKSUM_OFFSET) !=
      Crc32WithZeroedU32(bytes, CHECKSUM_OFFSET)) {
    return Err(Status::Corruption("page checksum mismatch"));
  }

  const auto entry_count = little_endian::GetU16(bytes, ENTRY_COUNT_OFFSET);
  const std::size_t slots_end = HEADER_SIZE + entry_count * SLOT_SIZE;
  if (slots_end > PAGE_SIZE) {
    return Err(Status::Corruption("invalid page slot count"));
  }

  std::string_view previous_key;
  for (std::size_t index = 0; index < entry_count; ++index) {
    const auto [begin, end] = CellBounds(page, entry_count, index);
    if (begin < slots_end || begin + KEY_SIZE_SIZE > end || end > PAGE_SIZE) {
      return Err(Status::Corruption("invalid cell offset"));
    }
    if (little_endian::GetU16(page, begin) > end - begin - KEY_SIZE_SIZE) {
      return Err(Status::Corruption("invalid key size"));
    }

    const auto cell = CellAt(page, entry_count, index);
    const std::string_view key = cell.key;
    if (index != 0 && previous_key >= key) {
      return Err(Status::Corruption("keys are not strictly ordered"));
    }
    previous_key = key;
    if (type == PageType::Internal &&
        (cell.value.size() != sizeof(PageId) ||
         !ValidLink(expected_page_id, little_endian::GetU32(cell.value, 0)))) {
      return Err(Status::Corruption("invalid internal child page ID"));
    }
  }

  const auto link = little_endian::GetU32(bytes, LINK_OFFSET);
  if (type == PageType::Leaf) {
    if (link != INVALID_PAGE_ID && !ValidLink(expected_page_id, link)) {
      return Err(Status::Corruption("invalid next leaf page ID"));
    }
  } else {
    if (!ValidLink(expected_page_id, link)) {
      return Err(Status::Corruption("invalid leftmost child page ID"));
    }
    if (entry_count == 0) {
      return Err(Status::Corruption("internal page has no entries"));
    }
  }
  return Page{page};
}

auto EncodeLeafPage(PageId page_id, PageId next_leaf,
                    std::span<const LeafEntry> entries) -> Result<PageBytes> {
  assert(ValidDataPageId(page_id));
  assert(next_leaf == INVALID_PAGE_ID || ValidLink(page_id, next_leaf));
  return EncodePage(PageType::Leaf, page_id, next_leaf, entries);
}

auto EncodeInternalPage(PageId page_id, PageId leftmost_child,
                        std::span<const InternalEntry> entries)
    -> Result<PageBytes> {
  assert(ValidDataPageId(page_id));
  assert(ValidLink(page_id, leftmost_child));
  assert(!entries.empty());
  return EncodePage(PageType::Internal, page_id, leftmost_child, entries);
}

} // namespace tinydb::storage
