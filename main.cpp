#include "tinydb/database.h"
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view HELP =
    "Commands:\n"
    "  put KEY VALUE    Store or replace a value\n"
    "  get KEY          Read a value\n"
    "  delete KEY       Remove a key\n"
    "  scan [START]     List keys and values in order, starting at START\n"
    "  checkpoint       Flush committed changes to the database file\n"
    "  help             Show these commands\n"
    "  quit             Exit\n"
    "Use double quotes for spaces or empty strings; escape quotes with \\\".\n";

auto Parse(std::string_view line) -> tinydb::Result<std::vector<std::string>> {
  std::istringstream input{std::string(line)};
  std::vector<std::string> args;
  while (input >> std::ws && !input.eof()) {
    std::string arg;
    if (!(input >> std::quoted(arg))) {
      return tinydb::Err(
          tinydb::Status::InvalidArgument("unterminated quoted argument"));
    }
    args.push_back(std::move(arg));
  }
  return args;
}

auto Run(tinydb::Database &database,
         const std::vector<std::string> &args) -> tinydb::Status {
  const auto &command = args.front();
  if (command == "put" && args.size() == 3) {
    if (auto status = database.Put(args[1], args[2]); !status.Ok()) {
      return status;
    }
    std::cout << "ok\n";
  } else if (command == "get" && args.size() == 2) {
    auto value = database.Get(args[1]);
    if (!value) {
      return std::move(value.error());
    }
    if (value->has_value()) {
      std::cout << std::quoted(**value) << '\n';
    } else {
      std::cout << "not found\n";
    }
  } else if (command == "delete" && args.size() == 2) {
    auto removed = database.Delete(args[1]);
    if (!removed) {
      return std::move(removed.error());
    }
    std::cout << (*removed ? "ok\n" : "not found\n");
  } else if (command == "scan" && args.size() <= 2) {
    auto reader = database.BeginRead();
    if (!reader) {
      return std::move(reader.error());
    }
    auto cursor = (*reader)->Seek(args.size() == 2 ? args[1] : "");
    if (!cursor) {
      return std::move(cursor.error());
    }
    while (cursor->Valid()) {
      std::cout << std::quoted(cursor->Key()) << ' '
                << std::quoted(cursor->Value()) << '\n';
      if (auto status = cursor->Next(); !status.Ok()) {
        return status;
      }
    }
  } else if (command == "checkpoint" && args.size() == 1) {
    if (auto status = database.Checkpoint(); !status.Ok()) {
      return status;
    }
    std::cout << "ok\n";
  } else if (command == "help" && args.size() == 1) {
    std::cout << HELP;
  } else {
    return tinydb::Status::InvalidArgument(
        "unknown command or wrong arguments; type help for usage");
  }
  return {};
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h")) {
    std::cout << "Usage: " << argv[0] << " DATABASE\n\n" << HELP;
    return 0;
  }
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " DATABASE\n";
    return 2;
  }
  auto database = tinydb::Database::Open(argv[1], 16);
  if (!database) {
    std::cerr << "error: " << database.error().Message() << '\n';
    return 1;
  }

  const bool interactive = isatty(STDIN_FILENO);
  if (interactive) {
    std::cout << "TinyDB. Type help for commands.\n";
  }
  int exit_code = 0;
  std::string line;
  while (true) {
    if (interactive) {
      std::cout << "tinydb> " << std::flush;
    }
    if (!std::getline(std::cin, line)) {
      break;
    }
    auto args = Parse(line);
    if (!args) {
      std::cerr << "error: " << args.error().Message() << '\n';
      exit_code = 1;
      continue;
    }
    if (args->empty()) {
      continue;
    }
    if (args->size() == 1 && args->front() == "quit") {
      break;
    }
    if (auto status = Run(**database, *args); !status.Ok()) {
      std::cerr << "error: " << status.Message() << '\n';
      exit_code = 1;
    }
  }
  if (std::cin.bad()) {
    std::cerr << "error: failed to read input\n";
    return 1;
  }
  return exit_code;
}
