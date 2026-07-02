#include <tinydb/storage_engine.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

/**
  Minimal command-line front end. Every invocation opens the database, runs
  one command, and closes it, so persistence is exercised on every call.

    TinyDB <db-file> put <key> <value>
    TinyDB <db-file> get <key>
    TinyDB <db-file> del <key>
    TinyDB <db-file> scan [<start> <end>]

  Exit codes: 0 on success, 1 on failure (missing key, oversized entry,
  I/O error), 2 on usage errors.
*/

namespace {

void PrintUsage(std::string_view program) {
  std::cerr << "usage: " << program << " <db-file> <command> [arguments]\n"
            << "commands:\n"
            << "  put <key> <value>    insert a row or replace its value\n"
            << "  get <key>            print the value stored under key\n"
            << "  del <key>            remove a key\n"
            << "  scan [<start> <end>] print rows with start <= key < end,\n"
            << "                       or every row when no bounds are given\n";
}

// An end bound greater than any storable key: keys are capped at
// MAX_ENTRY_BYTES, so one more byte of 0xff outranks them all.
auto ScanEverythingEnd() -> std::string {
  auto end = std::string(tinydb::MAX_ENTRY_BYTES + 1, '\xff');
  return end;
}

auto RunCommand(tinydb::StorageEngine &engine, std::string_view program,
                const std::vector<std::string_view> &args) -> int {
  const auto command = args[1];

  if (command == "put" && args.size() == 4) {
    switch (engine.Put(args[2], args[3])) {
      case tinydb::PutStatus::Ok:
        return 0;
      case tinydb::PutStatus::EntryTooLarge:
        std::cerr << "error: key + value exceeds " << tinydb::MAX_ENTRY_BYTES << " bytes\n";
        return 1;
      case tinydb::PutStatus::Closed:
        std::cerr << "error: database handle is closed\n";
        return 1;
    }
    return 1;
  }

  if (command == "get" && args.size() == 3) {
    const auto value = engine.Get(args[2]);
    if (!value.has_value()) {
      std::cerr << "error: key not found\n";
      return 1;
    }
    std::cout << *value << '\n';
    return 0;
  }

  if (command == "del" && args.size() == 3) {
    engine.Remove(args[2]);
    return 0;
  }

  if (command == "scan" && (args.size() == 2 || args.size() == 4)) {
    const auto rows = args.size() == 4 ? engine.Scan(args[2], args[3]) : engine.Scan("", ScanEverythingEnd());
    for (const auto &[key, value] : rows) {
      std::cout << key << '\t' << value << '\n';
    }
    return 0;
  }

  PrintUsage(program);
  return 2;
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const auto program = std::string_view{argc > 0 ? argv[0] : "TinyDB"};
  auto args = std::vector<std::string_view>{};
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  if (args.size() < 2) {
    PrintUsage(program);
    return 2;
  }

  try {
    auto engine = tinydb::StorageEngine::Open(std::filesystem::path{args[0]});
    return RunCommand(engine, program, args);
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
