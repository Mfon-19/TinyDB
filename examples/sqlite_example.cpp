#include "tinydb_vtab.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Check(sqlite3 *db, int code) {
  if (code != SQLITE_OK && code != SQLITE_ROW && code != SQLITE_DONE) {
    throw std::runtime_error(sqlite3_errmsg(db));
  }
}

void Exec(sqlite3 *db, const std::string &sql) {
  Check(db, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr));
}

// Prints each row of a query as tab-separated columns.
void Print(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *statement = nullptr;
  Check(db, sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr));
  int code = SQLITE_OK;
  while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
    std::cout << " ";
    for (int column = 0; column < sqlite3_column_count(statement); ++column) {
      const auto *text = sqlite3_column_text(statement, column);
      std::cout << ' '
                << (text ? reinterpret_cast<const char *>(text) : "NULL");
    }
    std::cout << '\n';
  }
  sqlite3_finalize(statement);
  Check(db, code);
}

// Opens the SQLite database that holds the schema and registers the module,
// which every connection needs before it can use TinyDB tables.
sqlite3 *Open(const std::string &path) {
  sqlite3 *db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    const std::string message = sqlite3_errmsg(db);
    sqlite3_close(db);
    throw std::runtime_error(message);
  }
  Check(db, tinydb_sqlite_register(db));
  return db;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " DIRECTORY\n";
    return 2;
  }

  sqlite3 *db = nullptr;
  try {
    const std::filesystem::path directory = argv[1];
    std::filesystem::create_directories(directory);
    const auto schema = (directory / "schema.db").string();
    const auto data = (directory / "data.tinydb").string();
    db = Open(schema);

    // SQLite stores the table definitions in schema.db; TinyDB stores the rows
    // in data.tinydb. Dropping a table also deletes its rows from TinyDB.
    Exec(db, "DROP TABLE IF EXISTS users");
    Exec(db, "DROP TABLE IF EXISTS orders");
    Exec(db, "CREATE VIRTUAL TABLE users USING tinydb('" + data +
                 "', id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)");
    Exec(db, "CREATE VIRTUAL TABLE orders USING tinydb('" + data +
                 "', id INTEGER PRIMARY KEY, user INTEGER, total REAL)");

    // Statements in one SQLite transaction commit as one TinyDB transaction,
    // even across tables. Bind values instead of formatting them into SQL.
    Exec(db, "BEGIN");
    sqlite3_stmt *insert = nullptr;
    Check(db, sqlite3_prepare_v2(db, "INSERT INTO users VALUES (?, ?, ?)", -1,
                                 &insert, nullptr));
    const struct {
      int id;
      const char *name;
      int age;
    } users[] = {{1, "alice", 31}, {2, "bob", 27}, {3, "carol", 45}};
    for (const auto &user : users) {
      sqlite3_bind_int(insert, 1, user.id);
      sqlite3_bind_text(insert, 2, user.name, -1, SQLITE_STATIC);
      sqlite3_bind_int(insert, 3, user.age);
      Check(db, sqlite3_step(insert));
      sqlite3_reset(insert);
    }
    sqlite3_finalize(insert);
    Exec(db, "INSERT INTO orders VALUES (10, 1, 5.0), (11, 2, 7.5), "
             "(12, 1, 1.25)");
    Exec(db, "COMMIT");

    // Conditions on the primary key become TinyDB lookups and range scans,
    // which return rows in key order.
    std::cout << "Users with id >= 2:\n";
    Print(db, "SELECT id, name, age FROM users WHERE id >= 2 ORDER BY id");

    // SQLite runs joins and aggregates over the rows TinyDB returns.
    std::cout << "Order totals per user:\n";
    Print(db, "SELECT name, sum(total) FROM users JOIN orders "
              "ON orders.user = users.id GROUP BY name ORDER BY name");

    // A statement that violates a constraint fails without undoing earlier
    // statements in the transaction.
    Exec(db, "BEGIN");
    Exec(db, "UPDATE users SET age = age + 1 WHERE id = 1");
    if (sqlite3_exec(db,
                     "INSERT INTO users VALUES (4, 'dave', 19), "
                     "(1, 'duplicate', 0)",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
      std::cout << "Insert failed: " << sqlite3_errmsg(db) << '\n';
    }
    Exec(db, "COMMIT");
    std::cout << "After commit, alice's age and whether dave exists:\n";
    Print(db, "SELECT (SELECT age FROM users WHERE id = 1), "
              "(SELECT count(*) FROM users WHERE id = 4)");

    // ROLLBACK discards the TinyDB transaction.
    Exec(db, "BEGIN");
    Exec(db, "DELETE FROM orders");
    Exec(db, "ROLLBACK");
    std::cout << "Orders after rolled-back delete:\n";
    Print(db, "SELECT count(*) FROM orders");

    // Reopening reconnects the tables to the same TinyDB file.
    sqlite3_close(db);
    db = Open(schema);
    std::cout << "After reopening:\n";
    Print(db, "SELECT id, name FROM users ORDER BY id");
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    sqlite3_close(db);
    return 1;
  }

  sqlite3_close(db);
  return 0;
}
