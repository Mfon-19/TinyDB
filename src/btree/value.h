#pragma once

#include "util/check.h"
#include "storage/page.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tinydb {

/*
** LEAF VALUE REPRESENTATION
**
** Inline values borrow bytes from their leaf. Overflow values store one fixed
** descriptor in the leaf and place the logical bytes in separately checksummed
** pages. The first page ID is also the value identifier recorded by every page
** in the chain; it prevents one valid page from being spliced into another
** value. value_checksum binds ordering and content across the complete chain.
*/
enum class LeafValueKind : std::uint8_t {
  Inline = 0,
  Overflow = 1,
};

struct OverflowValueDescriptor {
  std::uint64_t total_value_bytes{0};
  page_id_t first_page_id{HEADER_PAGE_ID};
  std::uint32_t value_checksum{0};

  auto operator==(const OverflowValueDescriptor &) const -> bool = default;
};

inline constexpr std::size_t OVERFLOW_VALUE_DESCRIPTOR_BYTES = 24;

class LeafValueView final {
 public:
  static auto Inline(std::string_view bytes) -> LeafValueView { return LeafValueView(bytes); }
  static auto Overflow(OverflowValueDescriptor descriptor) -> LeafValueView { return LeafValueView(descriptor); }

  auto Kind() const -> LeafValueKind { return kind_; }
  auto IsOverflow() const -> bool { return kind_ == LeafValueKind::Overflow; }
  auto Size() const -> std::uint64_t {
    return IsOverflow() ? overflow_.total_value_bytes : static_cast<std::uint64_t>(inline_bytes_.size());
  }

  auto InlineBytes() const -> std::string_view {
    TINYDB_CHECK(!IsOverflow(), "reading inline bytes from an overflow descriptor");
    return inline_bytes_;
  }
  auto OverflowDescriptor() const -> const OverflowValueDescriptor & {
    TINYDB_CHECK(IsOverflow(), "reading an overflow descriptor from an inline value");
    return overflow_;
  }

 private:
  explicit LeafValueView(std::string_view bytes) : inline_bytes_(bytes) {}
  explicit LeafValueView(OverflowValueDescriptor descriptor)
      : kind_(LeafValueKind::Overflow), overflow_(descriptor) {}

  LeafValueKind kind_{LeafValueKind::Inline};
  std::string_view inline_bytes_;
  OverflowValueDescriptor overflow_;
};

/* Owning form used only while a private leaf is being rebuilt. */
struct LeafValue {
  static auto Inline(std::string_view bytes) -> LeafValue {
    return LeafValue{
        .kind = LeafValueKind::Inline,
        .inline_bytes = std::string(bytes),
        .overflow = {},
    };
  }
  static auto Overflow(OverflowValueDescriptor descriptor) -> LeafValue {
    return LeafValue{
        .kind = LeafValueKind::Overflow,
        .inline_bytes = {},
        .overflow = descriptor,
    };
  }
  static auto Copy(LeafValueView value) -> LeafValue {
    return value.IsOverflow() ? Overflow(value.OverflowDescriptor()) : Inline(value.InlineBytes());
  }

  auto IsOverflow() const -> bool { return kind == LeafValueKind::Overflow; }
  auto EncodedBytes() const -> std::size_t {
    return IsOverflow() ? OVERFLOW_VALUE_DESCRIPTOR_BYTES : inline_bytes.size();
  }
  LeafValueKind kind{LeafValueKind::Inline};
  std::string inline_bytes;
  OverflowValueDescriptor overflow;
};

}  // namespace tinydb
