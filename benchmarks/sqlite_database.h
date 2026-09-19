#pragma once

#include "tinydb/status.h"
#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bench {

// Only the single-threaded benchmark uses this connection. Match TinyDB's
// result types so the same workload and validation code exercise both engines.
inline void SqliteCheck(sqlite3 *db, int code) {
  if (code != SQLITE_OK) {
    std::cerr << "benchmark failed: SQLite: " << sqlite3_errmsg(db) << '\n';
    std::exit(1);
  }
}

class Statement {
public:
  Statement(sqlite3 *db, const char *sql) {
    SqliteCheck(db, sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr));
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  auto operator=(const Statement &) -> Statement & = delete;

  void Bind(int index, std::string_view value) {
    SqliteCheck(
        sqlite3_db_handle(statement_),
        sqlite3_bind_blob(statement_, index, value.empty() ? "" : value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT));
  }
  bool Step() {
    const auto code = sqlite3_step(statement_);
    if (code != SQLITE_ROW && code != SQLITE_DONE) {
      SqliteCheck(sqlite3_db_handle(statement_), code);
    }
    return code == SQLITE_ROW;
  }
  std::string_view Column(int index) const {
    const auto *bytes = sqlite3_column_blob(statement_, index);
    return {bytes ? static_cast<const char *>(bytes) : "",
            static_cast<std::size_t>(sqlite3_column_bytes(statement_, index))};
  }
  void Reset() {
    SqliteCheck(sqlite3_db_handle(statement_), sqlite3_reset(statement_));
    SqliteCheck(sqlite3_db_handle(statement_),
                sqlite3_clear_bindings(statement_));
  }
  void Execute() {
    Step();
    Reset();
  }

private:
  sqlite3_stmt *statement_ = nullptr;
};

class SqliteDatabase {
public:
  class Cursor {
  public:
    explicit Cursor(Statement &statement)
        : statement_(&statement), valid_(statement.Step()) {}
    Cursor(Cursor &&other) noexcept
        : statement_(std::exchange(other.statement_, nullptr)),
          valid_(other.valid_) {}
    ~Cursor() {
      if (statement_) {
        statement_->Reset();
      }
    }
    bool Valid() const { return valid_; }
    std::string_view Key() const { return statement_->Column(0); }
    std::string_view Value() const { return statement_->Column(1); }
    tinydb::Status Next() {
      valid_ = statement_->Step();
      return {};
    }

  private:
    Statement *statement_;
    bool valid_ = false;
  };

  class Transaction {
  public:
    Transaction(SqliteDatabase &db, bool write) : db_(db) {
      (write ? *db_.begin_write_ : *db_.begin_read_).Execute();
    }
    Transaction(const Transaction &) = delete;
    auto operator=(const Transaction &) -> Transaction & = delete;
    ~Transaction() {
      if (active_) {
        db_.rollback_->Execute();
      }
    }
    tinydb::Result<std::optional<std::string>> Get(std::string_view key) {
      db_.get_->Bind(1, key);
      std::optional<std::string> value;
      if (db_.get_->Step()) {
        value = db_.get_->Column(0);
      }
      db_.get_->Reset();
      return value;
    }
    tinydb::Status Put(std::string_view key, std::string_view value) {
      db_.put_->Bind(1, key);
      db_.put_->Bind(2, value);
      db_.put_->Execute();
      return {};
    }
    tinydb::Result<bool> Delete(std::string_view key) {
      db_.delete_->Bind(1, key);
      db_.delete_->Execute();
      return sqlite3_changes(db_.db_.get()) != 0;
    }
    tinydb::Result<Cursor> Seek(std::string_view key) {
      db_.scan_->Bind(1, key);
      return Cursor(*db_.scan_);
    }
    tinydb::Status Commit() {
      db_.commit_->Execute();
      active_ = false;
      return {};
    }

  private:
    SqliteDatabase &db_;
    bool active_ = true;
  };

  static tinydb::Result<std::unique_ptr<SqliteDatabase>> Open(
      const std::string &path, std::size_t pool) {
    auto database = std::make_unique<SqliteDatabase>();
    sqlite3 *handle = nullptr;
    const auto code =
        sqlite3_open_v2(path.c_str(), &handle,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    database->db_.reset(handle);
    SqliteCheck(handle, code);
    const auto exec = [&](const std::string &sql) {
      SqliteCheck(handle,
                  sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr));
    };
    const auto setting = [&](const std::string &sql,
                             const std::string &expected) {
      Statement query(handle, sql.c_str());
      if (!query.Step() || query.Column(0) != expected) {
        std::cerr << "benchmark failed: SQLite setting: " << sql << '\n';
        std::exit(1);
      }
    };
    exec("PRAGMA page_size=4096");
    setting("PRAGMA journal_mode=WAL", "wal");
    exec("PRAGMA synchronous=FULL; PRAGMA mmap_size=0");
    exec(std::format("PRAGMA cache_size={}; PRAGMA wal_autocheckpoint={}", pool,
                     std::max<std::size_t>(1, pool / 2)));
    setting("PRAGMA page_size", "4096");
    setting("PRAGMA synchronous", "2");
    setting("PRAGMA mmap_size", "0");
    setting("PRAGMA cache_size", std::to_string(pool));
    setting("PRAGMA wal_autocheckpoint",
            std::to_string(std::max<std::size_t>(1, pool / 2)));
    exec("CREATE TABLE IF NOT EXISTS kv(key BLOB PRIMARY KEY, value BLOB NOT "
         "NULL) "
         "WITHOUT ROWID");
    const auto prepare = [&](const char *sql) {
      return std::make_unique<Statement>(handle, sql);
    };
    database->begin_read_ = prepare("BEGIN");
    database->begin_write_ = prepare("BEGIN IMMEDIATE");
    database->commit_ = prepare("COMMIT");
    database->rollback_ = prepare("ROLLBACK");
    database->get_ = prepare("SELECT value FROM kv WHERE key=?1");
    database->put_ = prepare("INSERT INTO kv VALUES(?1,?2) ON CONFLICT(key) "
                             "DO UPDATE SET value=excluded.value");
    database->delete_ = prepare("DELETE FROM kv WHERE key=?1");
    database->scan_ =
        prepare("SELECT key,value FROM kv WHERE key>=?1 ORDER BY key");
    return database;
  }
  tinydb::Result<std::unique_ptr<Transaction>> BeginRead() {
    return std::make_unique<Transaction>(*this, false);
  }
  tinydb::Result<std::unique_ptr<Transaction>> BeginWrite() {
    return std::make_unique<Transaction>(*this, true);
  }
  tinydb::Result<std::optional<std::string>> Get(std::string_view key) {
    Transaction reader(*this, false);
    return reader.Get(key);
  }
  tinydb::Status Checkpoint() {
    SqliteCheck(db_.get(), sqlite3_wal_checkpoint_v2(db_.get(), "main",
                                                     SQLITE_CHECKPOINT_TRUNCATE,
                                                     nullptr, nullptr));
    return {};
  }

private:
  // Statements must be finalized before the connection is closed.
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr,
                                                         sqlite3_close};
  std::unique_ptr<Statement> begin_read_, begin_write_, commit_, rollback_;
  std::unique_ptr<Statement> get_, put_, delete_, scan_;
};

} // namespace bench
