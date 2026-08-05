#pragma once

#include "storage/page.h"
#include "util/check.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tinydb {

/*
** LEAF VALUE REPRESENTATION
**
** A leaf cell stores a value in one of two forms.
**
** An inline cell contains the complete value bytes. LeafValueView borrows
** these bytes directly from the leaf page.
**
** An overflow cell contains a fixed 16-byte descriptor instead. The
** descriptor gives the total value size and the first overflow page ID. TinyDB
** stores the value bytes in a chain of overflow pages.
**
** The first overflow page ID also identifies the complete value. Every page
** repeats this ID as owner_value_id and stores its zero-based position as
** chunk_index.
** TinyDB rejects pages with an unexpected owner_value_id or chunk_index.
**
** Every page except the final page contains a full-size chunk. The final page
** contains exactly the remaining bytes. These rules reject a chain with the
** wrong total length or a short intermediate chunk. Each overflow page also
** has a checksum that covers the complete page.
*/
enum class LeafValueKind : std::uint8_t {
  Inline = 0,
  Overflow = 1,
};

struct OverflowValueDescriptor {
  std::uint64_t total_value_bytes{0};
  page_id_t first_page_id{HEADER_PAGE_ID};

  auto operator==(const OverflowValueDescriptor &) const -> bool = default;
};

inline constexpr std::size_t OVERFLOW_VALUE_DESCRIPTOR_BYTES = 16;

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

}  // namespace tinydb
