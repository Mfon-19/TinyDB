/*
 * Small driver for testing
 */

#include "tinydb/database.h"
#include <cstddef>
#include <iostream>
#include <string_view>

int main() {
  constexpr std::size_t BUFFER_POOL_CAPACITY = 16;
  auto opened = tinydb::Database::Open("my_db.db", BUFFER_POOL_CAPACITY);
  if (!opened) {
    std::cerr << opened.error().Message() << '\n';
    return 1;
  }
  tinydb::Database &db = **opened;

  if (auto status = db.Put("greeting", "konichiwaa"); !status.Ok()) {
    std::cerr << status.Message() << '\n';
    return 1;
  }

  auto value = db.Get("greeting");
  if (!value) {
    std::cerr << value.error().Message() << '\n';
    return 1;
  }
  std::cout << value->value_or("<missing>") << '\n';

  auto deleted = db.Delete("greeting");
  if (!deleted) {
    std::cerr << deleted.error().Message() << '\n';
    return 1;
  }
  std::cout << (*deleted ? "deleted" : "not found") << '\n';

  value = db.Get("greeting");
  if (!value) {
    std::cerr << value.error().Message() << '\n';
    return 1;
  }
  std::cout << value->value_or("<missing>") << '\n';
}
