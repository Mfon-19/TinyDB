/*
 * Small driver for testing
 */

#include "tinydb/database.h"
#include <algorithm>
#include <iostream>
#include <string_view>

int main() {
  auto db = tinydb::Database::Open("my_db.db");
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

  tinydb::storage::PageBytes read_page{};
  if (auto status = db->ReadPage(0, read_page); !status.Ok()) {
    std::cerr << status.Message() << '\n';
    return 1;
  }

  std::cout.write(read_page.data(),
                  static_cast<std::streamsize>(message.size()));
  std::cout << '\n';
}
