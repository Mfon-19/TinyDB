/*
 * Small driver for testing
 */

#include "tinydb/database.h"

int main() { 
  auto db = tinydb::Database(); 
  tinydb::Status status = db.Open("my_test_db");
}