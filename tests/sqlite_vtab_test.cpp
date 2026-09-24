#include "tinydb_vtab.h"
#include <cstdlib>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

namespace {

using Rows = std::vector<std::vector<std::string>>;

class SqliteVtabTest : public testing::Test {
protected:
  void SetUp() override {
    directory_ = testing::TempDir() + "tinydb_sqlite_XXXXXX";
    ASSERT_NE(mkdtemp(directory_.data()), nullptr);
    Open();
  }

  void TearDown() override {
    Close();
    std::filesystem::remove_all(directory_);
  }

  void Open() {
    ASSERT_EQ(sqlite3_open((directory_ + "/schema.db").c_str(), &db_),
              SQLITE_OK);
    ASSERT_EQ(tinydb_sqlite_register(db_), SQLITE_OK);
  }

  void Close() {
    sqlite3_close(db_);
    db_ = nullptr;
  }

  auto Path() const { return directory_ + "/data.tinydb"; }

  // Runs SQL; returns the SQLite result code.
  int Exec(const std::string &sql) {
    char *error = nullptr;
    const int code = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
    last_error_ = error ? error : "";
    sqlite3_free(error);
    return code;
  }

  void Run(const std::string &sql) {
    ASSERT_EQ(Exec(sql), SQLITE_OK) << sql << ": " << last_error_;
  }

  auto Query(const std::string &sql) -> Rows {
    Rows rows;
    sqlite3_stmt *statement = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr),
              SQLITE_OK)
        << sql << ": " << sqlite3_errmsg(db_);
    int code = SQLITE_OK;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
      auto &row = rows.emplace_back();
      for (int index = 0; index < sqlite3_column_count(statement); ++index) {
        const auto type = sqlite3_column_type(statement, index);
        const auto *text = sqlite3_column_text(statement, index);
        row.push_back(std::format(
            "{}:{}", type,
            text ? std::string(reinterpret_cast<const char *>(text),
                               static_cast<std::size_t>(
                                   sqlite3_column_bytes(statement, index)))
                 : "NULL"));
      }
    }
    EXPECT_EQ(code, SQLITE_DONE) << sql << ": " << sqlite3_errmsg(db_);
    sqlite3_finalize(statement);
    return rows;
  }

  auto Values(const std::string &sql) -> std::vector<std::string> {
    std::vector<std::string> values;
    for (const auto &row : Query(sql)) {
      values.push_back(row.at(0).substr(row.at(0).find(':') + 1));
    }
    return values;
  }

  void CreateTable(const std::string &name, const std::string &columns) {
    Run(std::format("CREATE VIRTUAL TABLE {} USING tinydb('{}', {})", name,
                    Path(), columns));
  }

  std::string directory_;
  sqlite3 *db_ = nullptr;
  std::string last_error_;
};

TEST_F(SqliteVtabTest, InsertsAndSelects) {
  CreateTable("users", "id INTEGER PRIMARY KEY, name TEXT, score REAL, "
                       "data BLOB");
  Run("INSERT INTO users VALUES (3, 'carol', 7.5, x'00ff'), "
      "(1, 'alice', NULL, NULL), (-2, 'bob', 1, 'text')");
  EXPECT_EQ(Values("SELECT id FROM users"),
            (std::vector<std::string>{"-2", "1", "3"}));
  EXPECT_EQ(Query("SELECT typeof(id), typeof(name), typeof(score), "
                  "typeof(data) FROM users WHERE id = 3"),
            (Rows{{"3:integer", "3:text", "3:real", "3:blob"}}));
  EXPECT_EQ(Values("SELECT name FROM users WHERE id > -2 AND id <= 3"),
            (std::vector<std::string>{"alice", "carol"}));
  EXPECT_EQ(Values("SELECT name FROM users WHERE name = 'bob'"),
            (std::vector<std::string>{"bob"}));
  EXPECT_EQ(Values("SELECT id FROM users ORDER BY id DESC"),
            (std::vector<std::string>{"3", "1", "-2"}));
  EXPECT_EQ(Values("SELECT count(*) FROM users WHERE score IS NULL"),
            (std::vector<std::string>{"1"}));
}

TEST_F(SqliteVtabTest, TextPrimaryKey) {
  CreateTable("words", "word TEXT PRIMARY KEY, n INTEGER");
  Run("INSERT INTO words VALUES ('pear', 1), ('apple', 2), ('fig', 3), "
      "(10, 4)");
  EXPECT_EQ(Values("SELECT word FROM words"),
            (std::vector<std::string>{"10", "apple", "fig", "pear"}));
  EXPECT_EQ(Values("SELECT word FROM words WHERE word >= 'b' AND word < 'p'"),
            (std::vector<std::string>{"fig"}));
  EXPECT_EQ(Values("SELECT n FROM words WHERE word = 10"),
            (std::vector<std::string>{"4"}));
}

TEST_F(SqliteVtabTest, UpdatesAndDeletes) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  Run("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd')");
  Run("UPDATE t SET v = v || '!' WHERE id >= 3");
  Run("UPDATE t SET id = id + 10 WHERE id = 1");
  Run("DELETE FROM t WHERE id = 2");
  EXPECT_EQ(Query("SELECT id, v FROM t"),
            (Rows{{"1:3", "3:c!"}, {"1:4", "3:d!"}, {"1:11", "3:a"}}));
  Run("DELETE FROM t");
  EXPECT_TRUE(Query("SELECT * FROM t").empty());
}

TEST_F(SqliteVtabTest, EnforcesPrimaryKeyConflicts) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  Run("INSERT INTO t VALUES (1, 'a'), (2, 'b')");
  EXPECT_EQ(Exec("INSERT INTO t VALUES (1, 'dup')") & 0xff, SQLITE_CONSTRAINT);
  EXPECT_EQ(Exec("UPDATE t SET id = 2 WHERE id = 1") & 0xff, SQLITE_CONSTRAINT);
  Run("INSERT OR IGNORE INTO t VALUES (1, 'ignored')");
  Run("INSERT OR REPLACE INTO t VALUES (2, 'replaced')");
  EXPECT_EQ(Exec("INSERT INTO t VALUES (NULL, 'x')") & 0xff, SQLITE_CONSTRAINT);
  EXPECT_EQ(Exec("INSERT INTO t VALUES ('abc', 'x')") & 0xff,
            SQLITE_CONSTRAINT);
  Run("INSERT OR IGNORE INTO t VALUES (2.5, 'x')");
  EXPECT_EQ(Query("SELECT id, v FROM t"),
            (Rows{{"1:1", "3:a"}, {"1:2", "3:replaced"}}));
}

TEST_F(SqliteVtabTest, EnforcesNotNullAndRejectsIgnoredConstraints) {
  CreateTable("t", "id INTEGER PRIMARY KEY, name TEXT NOT NULL, note TEXT");
  EXPECT_EQ(Exec("INSERT INTO t VALUES (1, NULL, 'x')") & 0xff,
            SQLITE_CONSTRAINT);
  Run("INSERT OR IGNORE INTO t VALUES (2, NULL, 'x')");
  Run("INSERT INTO t VALUES (3, 'named', NULL)");
  EXPECT_EQ(Values("SELECT id FROM t"), (std::vector<std::string>{"3"}));
  for (const auto *columns : {"id INTEGER PRIMARY KEY, n INTEGER CHECK(n > 0)",
                              "id INTEGER PRIMARY KEY, d TEXT DEFAULT 'x'",
                              "id INTEGER PRIMARY KEY, u TEXT UNIQUE",
                              "id TEXT PRIMARY KEY COLLATE NOCASE",
                              "a INTEGER, b INTEGER, "
                              "PRIMARY KEY(a, b)"}) {
    EXPECT_EQ(Exec(std::format("CREATE VIRTUAL TABLE bad USING tinydb('{}', "
                               "{})",
                               Path(), columns)),
              SQLITE_ERROR)
        << columns;
  }
}

TEST_F(SqliteVtabTest, FailedStatementRollsBackAlone) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  Run("BEGIN");
  Run("INSERT INTO t VALUES (1, 'kept')");
  EXPECT_EQ(Exec("INSERT INTO t VALUES (2, 'x'), (3, 'y'), (1, 'dup')") & 0xff,
            SQLITE_CONSTRAINT);
  Run("UPDATE t SET v = 'updated' WHERE id = 1");
  EXPECT_EQ(Exec("UPDATE t SET id = id + 1") & 0xff, SQLITE_OK);
  Run("COMMIT");
  EXPECT_EQ(Query("SELECT id, v FROM t"), (Rows{{"1:2", "3:updated"}}));
}

TEST_F(SqliteVtabTest, SavepointsAndRollback) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  Run("BEGIN");
  Run("INSERT INTO t VALUES (1, 'a')");
  Run("SAVEPOINT s1");
  Run("INSERT INTO t VALUES (2, 'b')");
  Run("UPDATE t SET v = 'changed' WHERE id = 1");
  Run("ROLLBACK TO s1");
  Run("INSERT INTO t VALUES (3, 'c')");
  Run("RELEASE s1");
  Run("COMMIT");
  EXPECT_EQ(Query("SELECT id, v FROM t"),
            (Rows{{"1:1", "3:a"}, {"1:3", "3:c"}}));
  Run("BEGIN");
  Run("DELETE FROM t");
  Run("ROLLBACK");
  EXPECT_EQ(Values("SELECT count(*) FROM t"), (std::vector<std::string>{"2"}));
}

TEST_F(SqliteVtabTest, PersistsAcrossReopen) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  Run("INSERT INTO t VALUES (1, 'durable')");
  Close();
  Open();
  EXPECT_EQ(Values("SELECT v FROM t WHERE id = 1"),
            (std::vector<std::string>{"durable"}));
}

TEST_F(SqliteVtabTest, RejectsOversizedRowsWithoutPoisoning) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  Run("BEGIN");
  Run("INSERT INTO t VALUES (1, 'small')");
  EXPECT_EQ(Exec("INSERT INTO t VALUES (2, zeroblob(2000))"), SQLITE_TOOBIG);
  Run("INSERT INTO t VALUES (3, 'after')");
  Run("COMMIT");
  EXPECT_EQ(Values("SELECT id FROM t"), (std::vector<std::string>{"1", "3"}));
}

TEST_F(SqliteVtabTest, JoinsTablesInOneFile) {
  CreateTable("users", "id INTEGER PRIMARY KEY, name TEXT");
  CreateTable("orders", "id INTEGER PRIMARY KEY, user INTEGER, total REAL");
  Run("BEGIN");
  Run("INSERT INTO users VALUES (1, 'alice'), (2, 'bob')");
  Run("INSERT INTO orders VALUES (10, 1, 5.0), (11, 2, 7.0), (12, 1, 1.5)");
  Run("COMMIT");
  EXPECT_EQ(Query("SELECT name, sum(total) FROM users JOIN orders "
                  "ON orders.user = users.id GROUP BY name ORDER BY name"),
            (Rows{{"3:alice", "2:6.5"}, {"3:bob", "2:7.0"}}));
}

TEST_F(SqliteVtabTest, DropRemovesRows) {
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  CreateTable("other", "id INTEGER PRIMARY KEY");
  Run("INSERT INTO t VALUES (1, 'a'), (2, 'b')");
  Run("INSERT INTO other VALUES (1)");
  Run("DROP TABLE t");
  CreateTable("t", "id INTEGER PRIMARY KEY, v TEXT");
  EXPECT_TRUE(Query("SELECT * FROM t").empty());
  EXPECT_EQ(Values("SELECT id FROM other"), (std::vector<std::string>{"1"}));
}

TEST_F(SqliteVtabTest, OpenReaderMakesWriterBusyInsteadOfDeadlocking) {
  CreateTable("t", "id INTEGER PRIMARY KEY");
  Run("INSERT INTO t VALUES (1), (2)");
  sqlite3_stmt *reader = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db_, "SELECT id FROM t", -1, &reader, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(reader), SQLITE_ROW);
  EXPECT_EQ(Exec("INSERT INTO t VALUES (3)"), SQLITE_BUSY);
  sqlite3_finalize(reader);
  Run("INSERT INTO t VALUES (3)");
  EXPECT_EQ(Values("SELECT count(*) FROM t"), (std::vector<std::string>{"3"}));
}

// Random statements against a TinyDB table and a native SQLite table with the
// same schema must leave both with the same contents and query results.
TEST_F(SqliteVtabTest, MatchesNativeTables) {
  for (const auto *type : {"INTEGER", "TEXT"}) {
    SCOPED_TRACE(type);
    const bool integer = std::string_view(type) == "INTEGER";
    Run("DROP TABLE IF EXISTS v");
    Run("DROP TABLE IF EXISTS n");
    // Keys are stored in their declared type, like a STRICT column; other
    // columns follow SQLite's affinity rules.
    CreateTable("v", std::format("k {} PRIMARY KEY, a, b TEXT, c INTEGER, "
                                 "d REAL, e NUMERIC",
                                 type));
    Run(std::format("CREATE TABLE n(k {} PRIMARY KEY CHECK(typeof(k) = '{}'), "
                    "a, b TEXT, c INTEGER, d REAL, e NUMERIC) WITHOUT ROWID",
                    type, integer ? "integer" : "text"));
    std::mt19937 random(integer ? 1 : 2);
    const auto key = [&] {
      const int value = static_cast<int>(random() % 40) - 20;
      switch (random() % 5) {
      case 0:
        return std::format("'{}'", value);
      case 1:
        return std::format("{}.5", value);
      case 2:
        return std::format("{}.0", value);
      default:
        return std::to_string(value);
      }
    };
    const auto value = [&] {
      switch (random() % 7) {
      case 0:
        return std::string("NULL");
      case 1:
        return std::format("{}.25", random() % 100);
      case 2:
        return std::format("'s{}'", random() % 100);
      case 3:
        return std::format("'{}'", random() % 100);
      case 4:
        return std::format("'{}.0'", random() % 100);
      case 5:
        return std::format("x'{:02x}'", random() % 256);
      default:
        return std::to_string(random() % 100);
      }
    };
    const char *ops[] = {"<", "<=", ">", ">=", "="};
    int nonempty = 0;
    for (int step = 0; step < 1500; ++step) {
      std::string statement;
      // Mostly inserts and single-key deletes, so the tables stay populated.
      switch (random() % 10) {
      case 0:
      case 1:
      case 2:
      case 3:
        statement = std::format(
            "INSERT OR {} INTO {{0}} VALUES ({}, {}, {}, {}, {}, {})",
            random() % 2 ? "REPLACE" : "IGNORE", key(), value(), value(),
            value(), value(), value());
        break;
      case 4:
        statement = std::format("UPDATE OR IGNORE {{0}} SET a = {}, k = {} "
                                "WHERE k = {}",
                                value(), key(), key());
        break;
      case 5:
        statement = std::format("DELETE FROM {{0}} WHERE k {} {}",
                                random() % 8 ? "=" : ops[random() % 5], key());
        break;
      default:
        statement =
            std::format("SELECT k, a, b, c, d, e FROM {{0}} WHERE k {} {} AND "
                        "k {} {} ORDER BY k",
                        ops[random() % 5], key(), ops[random() % 5], key());
      }
      const auto on = [&](const char *table) {
        return std::vformat(statement, std::make_format_args(table));
      };
      if (statement.starts_with("SELECT")) {
        const auto expected = Query(on("n"));
        nonempty += !expected.empty();
        ASSERT_EQ(Query(on("v")), expected) << on("v");
      } else {
        const int expected = Exec(on("n"));
        ASSERT_EQ(Exec(on("v")), expected) << on("v") << ": " << last_error_;
      }
    }
    ASSERT_EQ(Query("SELECT k, a, b, c, d, e FROM v"),
              Query("SELECT k, a, b, c, d, e FROM n ORDER BY k"));
    EXPECT_GT(Query("SELECT * FROM n").size(), 10U);
    EXPECT_GT(nonempty, 50);
  }
}

} // namespace
