#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tinydb {

/*
** PUBLIC BYTE-STRING MODEL
**
** TinyDB assigns no text encoding to keys or values. Bytes owns its storage;
** BytesView borrows caller storage only for the duration of the API call.
** Keys compare in unsigned lexicographic byte order, so ordering is identical
** on platforms where plain char has different signedness.
**
** These limits are enforced before a public mutation begins. Large values use
** overflow pages; keys remain bounded because internal B+ tree separators
** contain complete keys.
*/
using Bytes = std::string;
using BytesView = std::string_view;

inline constexpr std::size_t MAX_KEY_BYTES = 1024;
inline constexpr std::size_t MAX_VALUE_BYTES = 4U << 20U;

}  // namespace tinydb
