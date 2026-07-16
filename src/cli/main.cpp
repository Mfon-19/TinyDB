#include <tinydb/database.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

/*
** ONE-SHOT COMMAND-LINE FRONT END
**
** Each invocation opens one database, performs one public API operation, and
** closes it. The CLI owns parsing and presentation only; transaction,
** durability, and scan semantics remain those of Database. Exit status is 0
** on success, 1 for a database error or missing key, and 2 for invalid syntax.
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

auto ReportError(const tinydb::Status &status) -> int {
  std::cerr << "error: " << status.ToString() << '\n';
  return 1;
}

auto RunCommand(tinydb::Database &engine, std::string_view program, const std::vector<std::string_view> &args) -> int {
  const auto command = args[1];

  if (command == "put" && args.size() == 4) {
    if (const auto status = engine.Put(args[2], args[3]); !status.Ok()) {
      return ReportError(status);
    }
    return 0;
  }

  if (command == "get" && args.size() == 3) {
    const auto value = engine.Get(args[2]);
    if (!value) {
      return ReportError(value.error());
    }
    const auto &stored_value = value.value();
    if (!stored_value.has_value()) {
      std::cerr << "error: key not found\n";
      return 1;
    }
    std::cout << stored_value.value() << '\n';
    return 0;
  }

  if (command == "del" && args.size() == 3) {
    if (const auto status = engine.Delete(args[2]); !status.Ok()) {
      return ReportError(status);
    }
    return 0;
  }

  if (command == "scan" && (args.size() == 2 || args.size() == 4)) {
    auto transaction = engine.BeginRead();
    if (!transaction) {
      return ReportError(transaction.error());
    }
    auto cursor = args.size() == 4 ? transaction->Scan(tinydb::KeyRange::Between(args[2], args[3]))
                                   : transaction->Scan(tinydb::KeyRange::All());
    if (!cursor) {
      return ReportError(cursor.error());
    }
    while (cursor->Valid()) {
      const auto value = cursor->CopyValue();
      if (!value) {
        return ReportError(value.error());
      }
      std::cout << cursor->Key() << '\t' << *value << '\n';
      if (const auto status = cursor->Next(); !status.Ok()) {
        return ReportError(status);
      }
    }
    return 0;
  }

  PrintUsage(program);
  return 2;
}

}  // namespace

auto main(int argc, char **argv) -> int {
  try {
    const auto program = std::string_view{argc > 0 ? argv[0] : "TinyDB"};
    auto args = std::vector<std::string_view>{};
    for (int i = 1; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }

    if (args.size() < 2) {
      PrintUsage(program);
      return 2;
    }

    auto engine = tinydb::Database::Open(std::filesystem::path{args[0]});
    if (!engine) {
      return ReportError(engine.error());
    }
    const int code = RunCommand(*engine, program, args);

    // Commits are already durable. Explicit Close makes a final checkpoint
    // failure visible instead of relegating it to destructor diagnostics.
    if (const auto status = engine->Close(); !status.Ok()) {
      return ReportError(status);
    }
    return code;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
