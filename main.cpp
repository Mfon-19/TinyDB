/*
 * Small driver for testing
 */

#include "tinydb/database.h"
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string_view>

int main() {
  constexpr std::size_t BUFFER_POOL_CAPACITY = 16;
  auto db = tinydb::Database::Open("my_db.db", BUFFER_POOL_CAPACITY);
  if (!db) {
    std::cerr << db.error().Message() << '\n';
    return 1;
  }

  constexpr std::string_view message = "konichiwaa";
  tinydb::storage::PageBytes written_page{};
  std::ranges::copy(message, written_page.begin());

  if (auto status = db->WritePage(0, written_page); !status.Ok()) {
    std::cerr << status.Message() << '\n';
    return 1;
  }

  auto read_page = db->ReadPage(0);
  if (!read_page) {
    std::cerr << read_page.error().Message() << '\n';
    return 1;
  }

  std::cout.write(read_page->Bytes().data(),
                  static_cast<std::streamsize>(message.size()));
  std::cout << '\n';
}
