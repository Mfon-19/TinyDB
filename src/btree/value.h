#pragma once

#include "storage/page.h"
#include "util/check.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
    return IsOverflow() ? payload_.overflow.total_value_bytes
                        : static_cast<std::uint64_t>(payload_.inline_bytes.size());
  }

  auto InlineBytes() const -> std::string_view {
    TINYDB_CHECK(!IsOverflow(), "reading inline bytes from an overflow descriptor");
    return payload_.inline_bytes;
  }
  auto OverflowDescriptor() const -> const OverflowValueDescriptor & {
    TINYDB_CHECK(IsOverflow(), "reading an overflow descriptor from an inline value");
    return payload_.overflow;
  }

 private:
  union Payload {
    explicit Payload(std::string_view bytes) : inline_bytes(bytes) {}
    explicit Payload(OverflowValueDescriptor descriptor) : overflow(descriptor) {}

    std::string_view inline_bytes;
    OverflowValueDescriptor overflow;
  };

  explicit LeafValueView(std::string_view bytes) : payload_(bytes) {}
  explicit LeafValueView(OverflowValueDescriptor descriptor) : kind_(LeafValueKind::Overflow), payload_(descriptor) {}

  LeafValueKind kind_{LeafValueKind::Inline};
  Payload payload_;
};

/* Owning form used only while a private leaf is being rebuilt. */
class LeafValue final {
 public:
  static auto Inline(std::string_view bytes) -> LeafValue { return LeafValue(std::string(bytes)); }
  static auto Overflow(OverflowValueDescriptor descriptor) -> LeafValue { return LeafValue(descriptor); }
  static auto Copy(LeafValueView value) -> LeafValue {
    return value.IsOverflow() ? Overflow(value.OverflowDescriptor()) : Inline(value.InlineBytes());
  }

  auto IsOverflow() const -> bool { return std::holds_alternative<OverflowValueDescriptor>(payload_); }
  auto EncodedBytes() const -> std::size_t {
    return IsOverflow() ? OVERFLOW_VALUE_DESCRIPTOR_BYTES : std::get<std::string>(payload_).size();
  }

  auto InlineBytes() const -> std::string_view {
    TINYDB_CHECK(!IsOverflow(), "reading inline bytes from an owning overflow value");
    return std::get<std::string>(payload_);
  }

  auto OverflowDescriptor() const -> const OverflowValueDescriptor & {
    TINYDB_CHECK(IsOverflow(), "reading an overflow descriptor from an owning inline value");
    return std::get<OverflowValueDescriptor>(payload_);
  }

 private:
  explicit LeafValue(std::string bytes) : payload_(std::move(bytes)) {}
  explicit LeafValue(OverflowValueDescriptor descriptor) : payload_(descriptor) {}

  std::variant<std::string, OverflowValueDescriptor> payload_;
};

}  // namespace tinydb
