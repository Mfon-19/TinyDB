#include "tinydb/database.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// Convert TinyDB errors to exceptions to keep the example's flow readable.
void Check(tinydb::Status status) {
  if (!status.Ok()) {
    throw std::runtime_error(std::string{status.Message()});
  }
}

template <typename T> T Take(tinydb::Result<T> result) {
  if (!result) {
    throw std::runtime_error(std::string{result.error().Message()});
  }

  return std::move(*result);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " DATABASE\n";
    return 2;
  }

  try {
    auto database = Take(tinydb::Database::Open(argv[1], 64));

    // Commit several changes together. The writer can read its pending writes.
    {
      auto writer = Take(database->BeginWrite());
      Check(writer->Put("user:alice", "Alice"));
      Check(writer->Put("user:bob", "Bob"));
      Check(writer->Put("user:carol", "Carol"));

      std::cout << "Before commit: "
                << Take(writer->Get("user:alice")).value_or("(not found)")
                << '\n';
      Check(writer->Commit());
    }

    // Destroying a writer without Commit() rolls back its changes.
    {
      auto writer = Take(database->BeginWrite());
      Check(writer->Put("user:alice", "Uncommitted Alice"));
    }

    // Readers must finish before the next commit or checkpoint can complete.
    {
      auto reader = Take(database->BeginRead());
      auto value = Take(reader->Get("user:alice"));
      std::cout << "After rollback: " << value.value_or("(not found)") << '\n';

      // Seek finds the first key >= the requested key. Stop at the prefix end.
      // Keep the reader alive while using its cursor and consume views here.
      std::cout << "Users in key order:\n";
      auto cursor = Take(reader->Seek("user:"));
      while (cursor.Valid() && cursor.Key().starts_with("user:")) {
        std::cout << "  " << cursor.Key() << " = " << cursor.Value() << '\n';
        Check(cursor.Next());
      }
    }

    // Database::Delete commits a single write operation for you.
    const bool removed = Take(database->Delete("user:bob"));
    std::cout << "Deleted user:bob: " << (removed ? "yes" : "no") << '\n';

    // Commit is already durable. Checkpoint flushes committed pages to the
    // database file; closing and reopening below shows the persisted deletion.
    Check(database->Checkpoint());
    database.reset();
    database = Take(tinydb::Database::Open(argv[1], 64));

    std::cout << "After reopening, user:alice: "
              << Take(database->Get("user:alice")).value_or("(not found)")
              << '\n';

    // A successful Get returns an empty optional when the key is absent.
    auto value = Take(database->Get("user:bob"));
    std::cout << "After reopening, user:bob: " << value.value_or("(not found)")
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
