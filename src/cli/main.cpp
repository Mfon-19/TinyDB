#include "synarch/database.h"

#include <iostream>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: synarch <database-file>\n";
    return 2;
  }

  auto database = synarch::Database::open(argv[1]);
  if (!database) {
    std::cerr << "failed to open database: " << database.status().message() << '\n';
    return 1;
  }

  std::cout << "opened " << database.value().path() << '\n';
  return 0;
}
