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

  if (auto status = db.BeginWrite(); !status) {
    std::cerr << status.error().Message() << '\n';
    return 1;
  }

  db.Put("greeting1", "konichiwa");
  db.Put("greeting2", "morning");
}
