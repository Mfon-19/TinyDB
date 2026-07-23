#include "benchmark.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if defined(KVBENCH_TINYDB)
#include "api/database_test_access.h"

#include <tinydb/database.h>
#elif defined(KVBENCH_SQLITE)
#include <sqlite3.h>
#elif defined(KVBENCH_LEVELDB)
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#elif defined(KVBENCH_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#else
#error "No benchmark backend selected"
#endif

namespace tinydb::bench {
namespace {

auto HashBytes(std::string_view value) -> std::uint64_t {
  auto hash = std::uint64_t{1469598103934665603ULL};
  for (const auto byte : value) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

#if defined(KVBENCH_TINYDB)

class SelectedBackend final : public Backend {
 public:
  SelectedBackend(const std::filesystem::path &root, const Config &config, const Scenario &scenario) {
    std::filesystem::create_directories(root);
    auto options = Options{};
    if (config.page_cache_bytes) {
      options.page_cache_bytes = *config.page_cache_bytes;
    }
    const auto transaction_bytes =
        std::max<std::size_t>(32U << 20U, 3U * scenario.batch * (scenario.key_bytes + scenario.value_bytes));
    options.max_write_transaction_bytes = transaction_bytes;
    auto opened = Database::Open(root / "database.db", options);
    if (!opened) {
      Fail("open TinyDB backend: " + opened.error().ToString());
    }
    database_ = std::make_unique<Database>(std::move(*opened));
  }

  ~SelectedBackend() override {
    if (database_) {
      (void)database_->Close();
    }
  }

  void Put(std::span<const Entry> entries) override {
    auto transaction = database_->BeginWrite();
    if (!transaction) {
      Fail("begin TinyDB write: " + transaction.error().ToString());
    }
    for (const auto &entry : entries) {
      const auto status = transaction->Put(entry.key, entry.value);
      if (!status.Ok()) {
        Fail("put TinyDB value: " + status.ToString());
      }
    }
    auto committed = std::move(*transaction).Commit();
    if (!committed) {
      Fail("commit TinyDB write: " + committed.error().ToString());
    }
  }

  auto Get(std::string_view key) -> std::optional<std::string> override {
    auto value = database_->Get(key);
    if (!value) {
      Fail("get TinyDB value: " + value.error().ToString());
    }
    return std::move(*value);
  }

  void Delete(std::string_view key) override {
    const auto status = database_->Delete(key);
    if (!status.Ok()) {
      Fail("delete TinyDB value: " + status.ToString());
    }
  }

  auto Scan(std::optional<std::string_view> lower, std::size_t limit) -> ScanResult override {
    auto read = database_->BeginRead();
    if (!read) {
      Fail("begin TinyDB scan: " + read.error().ToString());
    }
    auto cursor = lower ? read->Scan(KeyRange::From(*lower)) : read->Scan();
    if (!cursor) {
      Fail("create TinyDB cursor: " + cursor.error().ToString());
    }
    auto result = ScanResult{};
    while (cursor->Valid() && result.rows < limit) {
      auto value = cursor->CopyValue();
      if (!value) {
        Fail("copy TinyDB cursor value: " + value.error().ToString());
      }
      result.digest ^= HashBytes(cursor->Key()) ^ HashBytes(*value);
      ++result.rows;
      const auto status = cursor->Next();
      if (!status.Ok()) {
        Fail("advance TinyDB cursor: " + status.ToString());
      }
    }
    return result;
  }

  void StabilizeFixture() override {
    const auto status = database_->Checkpoint();
    if (!status.Ok()) {
      Fail("checkpoint TinyDB fixture: " + status.ToString());
    }
  }

  void FinishReads() noexcept override { DatabaseTestAccess::WaitForReadQuiescence(*database_); }

 private:
  std::unique_ptr<Database> database_;
};

#elif defined(KVBENCH_SQLITE)

class SelectedBackend final : public Backend {
 public:
  SelectedBackend(const std::filesystem::path &root, const Config &config, const Scenario & /*scenario*/) {
    std::filesystem::create_directories(root);
    const auto path = root / "database.sqlite";
    Check(sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), "open");
    Exec(config.semantics == "durable" ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL");
    Exec(
        "CREATE TABLE IF NOT EXISTS kv "
        "(k BLOB PRIMARY KEY, v BLOB NOT NULL) WITHOUT ROWID");
    Prepare(
        "INSERT INTO kv(k,v) VALUES(?1,?2) "
        "ON CONFLICT(k) DO UPDATE SET v=excluded.v",
        &put_);
    Prepare("SELECT v FROM kv WHERE k=?1", &get_);
    Prepare("DELETE FROM kv WHERE k=?1", &delete_);
    Prepare("SELECT k,v FROM kv WHERE k>=?1 ORDER BY k LIMIT ?2", &scan_);
    Prepare("SELECT k,v FROM kv ORDER BY k LIMIT ?1", &scan_all_);
  }

  ~SelectedBackend() override {
    sqlite3_finalize(scan_all_);
    sqlite3_finalize(scan_);
    sqlite3_finalize(delete_);
    sqlite3_finalize(get_);
    sqlite3_finalize(put_);
    sqlite3_close(database_);
  }

  void Put(std::span<const Entry> entries) override {
    Exec("BEGIN IMMEDIATE");
    try {
      for (const auto &entry : entries) {
        Bind(put_, 1, entry.key);
        Bind(put_, 2, entry.value);
        StepDone(put_, "put");
      }
      Exec("COMMIT");
    } catch (...) {
      (void)sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
      throw;
    }
  }

  auto Get(std::string_view key) -> std::optional<std::string> override {
    Bind(get_, 1, key);
    const auto result = sqlite3_step(get_);
    if (result == SQLITE_DONE) {
      Reset(get_);
      return std::nullopt;
    }
    if (result != SQLITE_ROW) {
      const auto message = Error("get");
      Reset(get_);
      Fail(message);
    }
    const auto *data = static_cast<const char *>(sqlite3_column_blob(get_, 0));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(get_, 0));
    auto value = std::string{data, size};
    Reset(get_);
    return value;
  }

  void Delete(std::string_view key) override {
    Bind(delete_, 1, key);
    StepDone(delete_, "delete");
  }

  auto Scan(std::optional<std::string_view> lower, std::size_t limit) -> ScanResult override {
    auto *statement = lower ? scan_ : scan_all_;
    if (lower) {
      Bind(statement, 1, *lower);
      Check(sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(limit)), "bind scan limit");
    } else {
      Check(sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(limit)), "bind scan limit");
    }
    auto result = ScanResult{};
    while (true) {
      const auto step = sqlite3_step(statement);
      if (step == SQLITE_DONE) {
        break;
      }
      if (step != SQLITE_ROW) {
        const auto message = Error("scan");
        Reset(statement);
        Fail(message);
      }
      const auto key = std::string_view{static_cast<const char *>(sqlite3_column_blob(statement, 0)),
                                        static_cast<std::size_t>(sqlite3_column_bytes(statement, 0))};
      const auto value = std::string_view{static_cast<const char *>(sqlite3_column_blob(statement, 1)),
                                          static_cast<std::size_t>(sqlite3_column_bytes(statement, 1))};
      result.digest ^= HashBytes(key) ^ HashBytes(value);
      ++result.rows;
    }
    Reset(statement);
    return result;
  }

  void StabilizeFixture() override {}

 private:
  void Check(int result, std::string_view operation) const {
    if (result != SQLITE_OK) {
      Fail(Error(operation));
    }
  }

  auto Error(std::string_view operation) const -> std::string {
    return std::string(operation) + ": " + sqlite3_errmsg(database_);
  }

  void Exec(const char *sql) {
    auto *message = static_cast<char *>(nullptr);
    const auto result = sqlite3_exec(database_, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
      const auto detail = message ? std::string{message} : sqlite3_errmsg(database_);
      sqlite3_free(message);
      Fail(std::string{sql} + ": " + detail);
    }
  }

  void Prepare(const char *sql, sqlite3_stmt **statement) {
    Check(sqlite3_prepare_v2(database_, sql, -1, statement, nullptr), "prepare");
  }

  void Bind(sqlite3_stmt *statement, int index, std::string_view value) {
    Check(sqlite3_bind_blob(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT), "bind");
  }

  static void Reset(sqlite3_stmt *statement) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
  }

  void StepDone(sqlite3_stmt *statement, std::string_view operation) {
    const auto result = sqlite3_step(statement);
    if (result != SQLITE_DONE) {
      const auto message = Error(operation);
      Reset(statement);
      Fail(message);
    }
    Reset(statement);
  }

  sqlite3 *database_{nullptr};
  sqlite3_stmt *put_{nullptr};
  sqlite3_stmt *get_{nullptr};
  sqlite3_stmt *delete_{nullptr};
  sqlite3_stmt *scan_{nullptr};
  sqlite3_stmt *scan_all_{nullptr};
};

#elif defined(KVBENCH_LEVELDB) || defined(KVBENCH_ROCKSDB)

#if defined(KVBENCH_LEVELDB)
namespace lsm = leveldb;
constexpr auto kLsmDirectory = "leveldb";
#else
namespace lsm = rocksdb;
constexpr auto kLsmDirectory = "rocksdb";
#endif

void CheckLsm(const lsm::Status &status, std::string_view operation) {
  if (!status.ok()) {
    Fail(std::string(operation) + ": " + status.ToString());
  }
}

auto OpenLsm(const lsm::Options &options, const std::string &path) -> std::unique_ptr<lsm::DB> {
  auto database = std::unique_ptr<lsm::DB>{};
#if defined(KVBENCH_LEVELDB)
  auto *raw = static_cast<lsm::DB *>(nullptr);
  CheckLsm(lsm::DB::Open(options, path, &raw), "open");
  database.reset(raw);
#else
  CheckLsm(lsm::DB::Open(options, path, &database), "open");
#endif
  return database;
}

void StabilizeLsm(lsm::DB &database) {
#if defined(KVBENCH_LEVELDB)
  database.CompactRange(nullptr, nullptr);
#else
  auto options = lsm::FlushOptions{};
  options.wait = true;
  CheckLsm(database.Flush(options), "flush fixture");
#endif
}

class SelectedBackend final : public Backend {
 public:
  SelectedBackend(const std::filesystem::path &root, const Config &config, const Scenario & /*scenario*/)
      : durable_(config.semantics == "durable") {
    std::filesystem::create_directories(root);
    auto options = lsm::Options{};
    options.create_if_missing = true;
    database_ = OpenLsm(options, (root / kLsmDirectory).string());
  }

  void Put(std::span<const Entry> entries) override {
    auto batch = lsm::WriteBatch{};
    for (const auto &entry : entries) {
      batch.Put(lsm::Slice{entry.key.data(), entry.key.size()}, lsm::Slice{entry.value.data(), entry.value.size()});
    }
    auto options = lsm::WriteOptions{};
    options.sync = durable_;
    CheckLsm(database_->Write(options, &batch), "write");
  }

  auto Get(std::string_view key) -> std::optional<std::string> override {
    auto value = std::string{};
    const auto status = database_->Get(lsm::ReadOptions{}, lsm::Slice{key.data(), key.size()}, &value);
    if (status.IsNotFound()) {
      return std::nullopt;
    }
    CheckLsm(status, "get");
    return value;
  }

  void Delete(std::string_view key) override {
    auto options = lsm::WriteOptions{};
    options.sync = durable_;
    CheckLsm(database_->Delete(options, lsm::Slice{key.data(), key.size()}), "delete");
  }

  auto Scan(std::optional<std::string_view> lower, std::size_t limit) -> ScanResult override {
    auto iterator = std::unique_ptr<lsm::Iterator>{database_->NewIterator(lsm::ReadOptions{})};
    if (lower) {
      iterator->Seek(lsm::Slice{lower->data(), lower->size()});
    } else {
      iterator->SeekToFirst();
    }
    auto result = ScanResult{};
    while (iterator->Valid() && result.rows < limit) {
      const auto key = std::string_view{iterator->key().data(), iterator->key().size()};
      const auto value = std::string_view{iterator->value().data(), iterator->value().size()};
      result.digest ^= HashBytes(key) ^ HashBytes(value);
      ++result.rows;
      iterator->Next();
    }
    CheckLsm(iterator->status(), "scan");
    return result;
  }

  void StabilizeFixture() override { StabilizeLsm(*database_); }

 private:
  bool durable_{true};
  std::unique_ptr<lsm::DB> database_;
};

#endif

}  // namespace

auto Identity() -> BackendIdentity {
#if defined(KVBENCH_TINYDB)
  return {"tinydb", KVBENCH_TINYDB_FORMAT_FAMILY, true, true};
#elif defined(KVBENCH_SQLITE)
  return {"sqlite", "sqlite", false, false};
#elif defined(KVBENCH_LEVELDB)
  return {"leveldb", "leveldb", false, false};
#elif defined(KVBENCH_ROCKSDB)
  return {"rocksdb", "rocksdb", false, false};
#endif
}

auto OpenBackend(const std::filesystem::path &root, const Config &config,
                 const Scenario &scenario) -> std::unique_ptr<Backend> {
  return std::make_unique<SelectedBackend>(root, config, scenario);
}

}  // namespace tinydb::bench
