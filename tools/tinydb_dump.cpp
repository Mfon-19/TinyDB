#include <tinydb/database.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

auto Hex(std::string_view bytes) -> std::string {
  auto output = std::ostringstream{};
  output << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    output << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(byte));
  }
  return output.str();
}

auto Error(const tinydb::Status &status) -> int {
  std::cerr << "error: " << status.ToString() << '\n';
  return 1;
}

}  // namespace

auto main(int argc, char **argv) -> int {
  if (argc != 2) {
    std::cerr << "usage: tinydb_dump <database>\n";
    return 2;
  }
  auto database = tinydb::Database::Open(std::filesystem::path{argv[1]});
  if (!database) {
    return Error(database.error());
  }
  const auto verified = database->Verify();
  if (!verified) {
    return Error(verified.error());
  }
  if (!verified->Ok()) {
    std::cerr << "error: verification found " << verified->issues.size() << " issue(s)\n";
    return 1;
  }

  auto transaction = database->BeginRead();
  if (!transaction) {
    return Error(transaction.error());
  }
  auto cursor = transaction->Scan(tinydb::KeyRange::All());
  if (!cursor) {
    return Error(cursor.error());
  }
  while (cursor->Valid()) {
    auto value = cursor->CopyValue();
    if (!value) {
      return Error(value.error());
    }
    // Hex encoding makes every byte-string key and value line-oriented and
    // reversible, including tabs, newlines, NUL bytes, and invalid UTF-8.
    std::cout << Hex(cursor->Key()) << '\t' << Hex(*value) << '\n';
    if (auto status = cursor->Next(); !status.Ok()) {
      return Error(status);
    }
  }
  return 0;
}
