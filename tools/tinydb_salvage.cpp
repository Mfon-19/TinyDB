#include "salvage/salvage.h"

#include <filesystem>
#include <iostream>

auto main(int argc, char **argv) -> int {
  if (argc != 3) {
    std::cerr << "usage: tinydb_salvage <damaged-source> <new-destination>\n";
    return 2;
  }
  const auto report = tinydb::salvage::Run(std::filesystem::path{argv[1]}, std::filesystem::path{argv[2]});
  if (!report) {
    std::cerr << "error: " << report.error().ToString() << '\n';
    return 1;
  }
  std::cout << "pages scanned: " << report->pages_scanned << '\n'
            << "valid leaf pages: " << report->valid_leaf_pages << '\n'
            << "rows recovered: " << report->rows_recovered << '\n'
            << "duplicate rows: " << report->duplicate_rows << '\n'
            << "damaged pages: " << report->damaged_pages << '\n'
            << "damaged values: " << report->damaged_values << '\n'
            << "superblock: " << (report->superblock_available ? "available" : "unavailable") << '\n'
            << "allocator filter: " << (report->allocator_filter_available ? "available" : "unavailable") << '\n';
  return 0;
}
