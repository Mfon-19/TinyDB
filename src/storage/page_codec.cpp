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
inline constexpr std::size_t LEAF_CELL_HEADER_SIZE = sizeof(std::uint16_t);
inline constexpr std::size_t INTERNAL_CELL_HEADER_SIZE =
    sizeof(std::uint16_t) + sizeof(std::uint32_t);

struct PageHeader {
  std::uint16_t entry_count;
  PageId link;
};

auto ValidDataPageId(PageId page_id) noexcept -> bool {
  return page_id != 0 && page_id != INVALID_PAGE_ID;
}

auto SlotOffset(const PageBytes &page,
                std::size_t index) noexcept -> std::uint16_t {
  return little_endian::GetU16(page, HEADER_SIZE + index * SLOT_SIZE);
}

auto CellEnd(const PageBytes &page, std::uint16_t entry_count,
             std::size_t index) noexcept -> std::size_t {
  if (index + 1 < entry_count) {
    return SlotOffset(page, index + 1);
  }
  return PAGE_SIZE;
}

auto LeafEntriesSize(std::span<const LeafEntry> entries)
    -> Result<std::size_t> {
  if (entries.size() > (PAGE_SIZE - HEADER_SIZE) / SLOT_SIZE) {
    return Err(Status::InvalidArgument("leaf entries do not fit in a page"));
  }

  std::size_t size = HEADER_SIZE + entries.size() * SLOT_SIZE;
  for (const auto &entry : entries) {
    if (LEAF_CELL_HEADER_SIZE > PAGE_SIZE - size ||
        entry.key.size() > PAGE_SIZE - size - LEAF_CELL_HEADER_SIZE) {
      return Err(Status::InvalidArgument("leaf entries do not fit in a page"));
    }
    size += LEAF_CELL_HEADER_SIZE + entry.key.size();
    if (entry.value.size() > PAGE_SIZE - size) {
      return Err(Status::InvalidArgument("leaf entries do not fit in a page"));
    }
    size += entry.value.size();
  }
  return size;
}

auto InternalEntriesSize(std::span<const InternalEntry> entries)
    -> Result<std::size_t> {
  if (entries.size() > (PAGE_SIZE - HEADER_SIZE) / SLOT_SIZE) {
    return Err(
        Status::InvalidArgument("internal entries do not fit in a page"));
  }

  std::size_t size = HEADER_SIZE + entries.size() * SLOT_SIZE;
  for (const auto &entry : entries) {
    if (INTERNAL_CELL_HEADER_SIZE > PAGE_SIZE - size ||
        entry.key.size() > PAGE_SIZE - size - INTERNAL_CELL_HEADER_SIZE) {
      return Err(
          Status::InvalidArgument("internal entries do not fit in a page"));
    }
    size += INTERNAL_CELL_HEADER_SIZE + entry.key.size();
  }
  return size;
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

auto DecodeHeader(PageId expected_page_id, PageType expected_type,
                  const PageBytes &page) -> Result<PageHeader> {
  assert(ValidDataPageId(expected_page_id));

  const std::span<const char> bytes{page};
  if (!std::ranges::equal(PAGE_MAGIC,
                          bytes.subspan(MAGIC_OFFSET, PAGE_MAGIC.size()))) {
    return Err(Status::Corruption("invalid page magic"));
  }
  if (little_endian::GetU16(bytes, VERSION_OFFSET) != FORMAT_VERSION) {
    return Err(Status::Corruption("unsupported page version"));
  }
  if (little_endian::GetU16(bytes, TYPE_OFFSET) !=
      static_cast<std::uint16_t>(expected_type)) {
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

  if (entry_count != 0) {
    std::size_t previous = SlotOffset(page, 0);
    if (previous < slots_end || previous >= PAGE_SIZE) {
      return Err(Status::Corruption("invalid cell offset"));
    }

    for (std::size_t index = 1; index < entry_count; ++index) {
      const auto offset = SlotOffset(page, index);
      if (offset <= previous || offset >= PAGE_SIZE) {
        return Err(Status::Corruption("invalid cell offset"));
      }
      previous = offset;
    }
  }

  return PageHeader{entry_count, little_endian::GetU32(bytes, LINK_OFFSET)};
}

auto ValidateLeafEntries(const PageBytes &page,
                         std::uint16_t entry_count) -> Status {
  std::string_view previous_key;

  for (std::size_t index = 0; index < entry_count; ++index) {
    const std::size_t begin = SlotOffset(page, index);
    const std::size_t end = CellEnd(page, entry_count, index);
    if (end - begin < LEAF_CELL_HEADER_SIZE) {
      return Status::Corruption("invalid leaf cell size");
    }

    const auto key_size = little_endian::GetU16(page, begin);
    if (key_size > end - begin - LEAF_CELL_HEADER_SIZE) {
      return Status::Corruption("invalid leaf key size");
    }

    const std::string_view key{page.data() + begin + LEAF_CELL_HEADER_SIZE,
                               key_size};
    if (index != 0 && !(previous_key < key)) {
      return Status::Corruption("leaf keys are not strictly ordered");
    }
    previous_key = key;
  }

  return {};
}

auto ValidateInternalEntries(const PageBytes &page, std::uint16_t entry_count,
                             PageId page_id) -> Status {
  if (entry_count == 0) {
    return Status::Corruption("internal page has no entries");
  }

  std::string_view previous_key;

  for (std::size_t index = 0; index < entry_count; ++index) {
    const std::size_t begin = SlotOffset(page, index);
    const std::size_t end = CellEnd(page, entry_count, index);
    if (end - begin < INTERNAL_CELL_HEADER_SIZE) {
      return Status::Corruption("invalid internal cell size");
    }

    const auto key_size = little_endian::GetU16(page, begin);
    if (key_size != end - begin - INTERNAL_CELL_HEADER_SIZE) {
      return Status::Corruption("invalid internal key size");
    }

    const auto right_child =
        little_endian::GetU32(page, begin + sizeof(std::uint16_t));
    if (!ValidDataPageId(right_child) || right_child == page_id) {
      return Status::Corruption("invalid internal child page ID");
    }

    const std::string_view key{page.data() + begin + INTERNAL_CELL_HEADER_SIZE,
                               key_size};
    if (index != 0 && !(previous_key < key)) {
      return Status::Corruption("internal keys are not strictly ordered");
    }
    previous_key = key;
  }

  return {};
}

} // namespace

auto LeafPageView::Entry(std::size_t index) const noexcept -> LeafEntry {
  assert(index < entry_count_);
  const std::size_t begin = SlotOffset(*page_, index);
  const std::size_t end = CellEnd(*page_, entry_count_, index);
  const auto key_size = little_endian::GetU16(*page_, begin);
  const std::size_t key_begin = begin + LEAF_CELL_HEADER_SIZE;
  const std::size_t value_begin = key_begin + key_size;
  return {{page_->data() + key_begin, key_size},
          {page_->data() + value_begin, end - value_begin}};
}

auto InternalPageView::Entry(std::size_t index) const noexcept
    -> InternalEntry {
  assert(index < entry_count_);
  const std::size_t begin = SlotOffset(*page_, index);
  const auto key_size = little_endian::GetU16(*page_, begin);
  const auto right_child =
      little_endian::GetU32(*page_, begin + sizeof(std::uint16_t));
  return {{page_->data() + begin + INTERNAL_CELL_HEADER_SIZE, key_size},
          right_child};
}

auto PeekPageType(const PageBytes &page) noexcept -> PageType {
  return static_cast<PageType>(little_endian::GetU16(page, TYPE_OFFSET));
}

auto EncodeLeafPage(PageId page_id, PageId next_leaf,
                    std::span<const LeafEntry> entries) -> Result<PageBytes> {
  assert(ValidDataPageId(page_id));
  assert(next_leaf == INVALID_PAGE_ID ||
         (ValidDataPageId(next_leaf) && next_leaf != page_id));

  auto size = LeafEntriesSize(entries);
  if (!size) {
    return Err(std::move(size.error()));
  }

  PageBytes page{};
  std::size_t free_end = PAGE_SIZE;
  for (std::size_t index = entries.size(); index != 0; --index) {
    const auto &entry = entries[index - 1];
    const std::size_t cell_size =
        LEAF_CELL_HEADER_SIZE + entry.key.size() + entry.value.size();
    free_end -= cell_size;
    little_endian::PutU16(page, HEADER_SIZE + (index - 1) * SLOT_SIZE,
                          static_cast<std::uint16_t>(free_end));
    little_endian::PutU16(page, free_end,
                          static_cast<std::uint16_t>(entry.key.size()));
    std::ranges::copy(entry.key,
                      page.begin() + free_end + LEAF_CELL_HEADER_SIZE);
    std::ranges::copy(entry.value, page.begin() + free_end +
                                       LEAF_CELL_HEADER_SIZE +
                                       entry.key.size());
  }

  EncodeHeader(page, PageType::Leaf, page_id,
               static_cast<std::uint16_t>(entries.size()), next_leaf);
  return page;
}

auto DecodeLeafPage(PageId expected_page_id,
                    const PageBytes &page) -> Result<LeafPageView> {
  auto header = DecodeHeader(expected_page_id, PageType::Leaf, page);
  if (!header) {
    return Err(std::move(header.error()));
  }
  if (header->link != INVALID_PAGE_ID &&
      (!ValidDataPageId(header->link) || header->link == expected_page_id)) {
    return Err(Status::Corruption("invalid next leaf page ID"));
  }

  auto status = ValidateLeafEntries(page, header->entry_count);
  if (!status.Ok()) {
    return Err(std::move(status));
  }

  return LeafPageView{&page, expected_page_id, header->link,
                      header->entry_count};
}

auto EncodeInternalPage(PageId page_id, PageId leftmost_child,
                        std::span<const InternalEntry> entries)
    -> Result<PageBytes> {
  assert(ValidDataPageId(page_id));
  assert(ValidDataPageId(leftmost_child) && leftmost_child != page_id);
  assert(!entries.empty());

  auto size = InternalEntriesSize(entries);
  if (!size) {
    return Err(std::move(size.error()));
  }

  PageBytes page{};
  std::size_t free_end = PAGE_SIZE;
  for (std::size_t index = entries.size(); index != 0; --index) {
    const auto &entry = entries[index - 1];
    const std::size_t cell_size = INTERNAL_CELL_HEADER_SIZE + entry.key.size();
    free_end -= cell_size;
    little_endian::PutU16(page, HEADER_SIZE + (index - 1) * SLOT_SIZE,
                          static_cast<std::uint16_t>(free_end));
    little_endian::PutU16(page, free_end,
                          static_cast<std::uint16_t>(entry.key.size()));
    little_endian::PutU32(page, free_end + sizeof(std::uint16_t),
                          entry.right_child);
    std::ranges::copy(entry.key,
                      page.begin() + free_end + INTERNAL_CELL_HEADER_SIZE);
  }

  EncodeHeader(page, PageType::Internal, page_id,
               static_cast<std::uint16_t>(entries.size()), leftmost_child);
  return page;
}

auto DecodeInternalPage(PageId expected_page_id,
                        const PageBytes &page) -> Result<InternalPageView> {
  auto header = DecodeHeader(expected_page_id, PageType::Internal, page);
  if (!header) {
    return Err(std::move(header.error()));
  }
  if (!ValidDataPageId(header->link) || header->link == expected_page_id) {
    return Err(Status::Corruption("invalid leftmost child page ID"));
  }

  auto status =
      ValidateInternalEntries(page, header->entry_count, expected_page_id);
  if (!status.Ok()) {
    return Err(std::move(status));
  }

  return InternalPageView{&page, expected_page_id, header->link,
                          header->entry_count};
}

} // namespace tinydb::storage
