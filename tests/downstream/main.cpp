#include <tinydb/bytes.h>
#include <tinydb/cursor.h>
#include <tinydb/database.h>
#include <tinydb/options.h>
#include <tinydb/stats.h>
#include <tinydb/status.h>
#include <tinydb/transaction.h>

#include <filesystem>
#include <type_traits>

static_assert(std::is_move_constructible_v<tinydb::Database>);
static_assert(!std::is_move_assignable_v<tinydb::Database>);
static_assert(!std::is_copy_constructible_v<tinydb::Database>);

auto main() -> int {
  auto options = tinydb::Options{};
  options.page_cache_bytes = 512U << 10U;

  // These representative symbols force the installed headers and exported
  // target to compile and link without access to the source tree.
  const auto open = &tinydb::Database::Open;
  const auto all = tinydb::KeyRange::All();
  const auto bytes = tinydb::Bytes{"value"};
  static_cast<void>(options);
  static_cast<void>(open);
  static_cast<void>(all);
  static_cast<void>(bytes);
  return 0;
}
