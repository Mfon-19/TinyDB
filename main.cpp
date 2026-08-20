/*
 * Small driver for testing
 */

#include "tinydb/database.h"
#include <iostream>
#include <string>

int main() { 
  auto db = tinydb::Database::Open("my_db.db"); 
  if (!db) {
    std::cerr << db.error().Message() << '\n';
    return 1;
  }

  db->Write("konichiwaa");
  std::string buffer(20, '\0');
  db->Read(buffer);
  std::cout << "returned : " << buffer << " from db \n";
}