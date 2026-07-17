#include <tinydb/database.h>

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

auto allocation_count = std::atomic<std::uint64_t>{0};
auto count_allocations = std::atomic<bool>{false};

}  // namespace

void *operator new(std::size_t bytes) {
  if (count_allocations.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (auto *memory = std::malloc(bytes); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

void *operator new[](std::size_t bytes) { return ::operator new(bytes); }

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete[](void *memory) noexcept { std::free(memory); }

void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using Clock = std::chrono::steady_clock;

/*
** GUARANTEE-LEVEL BENCHMARK MODEL
**
** TinyDB and LevelDB use the same 256 KiB cache target, deterministic keys,
** values, batch geometry, disabled compression, and synchronous durability.
** Rows named for both engines are directly comparable. TinyDB-only rows expose
** mechanisms with no LevelDB API equivalent: WAL amplification, checkpoint
** transfer, and reader-delayed publication.
**
** Setup is excluded from point-read and cursor timing. Transaction throughput
** includes transaction construction and mutation preparation; latency columns
** time only the durability call. Results are CSV so a run records its complete
** measurement contract without embedding machine-specific claims in source.
*/
struct Config {
  std::size_t rows{2'000};
  std::size_t transactions{100};
  std::size_t batch{16};
  std::size_t value_bytes{100};
  std::size_t churn_rounds{4};
};

struct Metrics {
  std::string engine;
  std::string workload;
  std::uint64_t operations{0};
  double seconds{0};
  double p50_us{std::numeric_limits<double>::quiet_NaN()};
  double p95_us{std::numeric_limits<double>::quiet_NaN()};
  double p99_us{std::numeric_limits<double>::quiet_NaN()};
  std::uint64_t app_bytes{0};
  std::uint64_t wal_bytes{0};
  double wal_bytes_per_app_byte{std::numeric_limits<double>::quiet_NaN()};
  double checkpoint_ms{std::numeric_limits<double>::quiet_NaN()};
  double checkpoint_mib_per_second{std::numeric_limits<double>::quiet_NaN()};
  double allocations_per_operation{std::numeric_limits<double>::quiet_NaN()};
  double cache_hit_rate{std::numeric_limits<double>::quiet_NaN()};
  double recovery_ms{std::numeric_limits<double>::quiet_NaN()};
  std::uint64_t file_bytes{0};
};

[[noreturn]] void Fail(std::string_view message) {
  std::fprintf(stderr, "benchmark failed: %.*s\n", static_cast<int>(message.size()), message.data());
  std::exit(1);
}

void Check(const tinydb::Status &status, std::string_view operation) {
  if (!status.Ok()) {
    Fail(std::string(operation) + ": " + status.ToString());
  }
}

void Check(const leveldb::Status &status, std::string_view operation) {
  if (!status.ok()) {
    Fail(std::string(operation) + ": " + status.ToString());
  }
}

template <typename T>
auto Take(tinydb::Result<T> result, std::string_view operation) -> T {
  if (!result) {
    Fail(std::string(operation) + ": " + result.error().ToString());
  }
  return std::move(*result);
}

auto Key(std::size_t row) -> std::string {
  auto key = std::to_string(row);
  key.insert(key.begin(), 12U - key.size(), '0');
  return key;
}

auto Value(std::size_t row, std::size_t bytes) -> std::string {
  auto value = std::string(bytes, static_cast<char>('a' + row % 26U));
  const auto prefix = std::to_string(row) + ':';
  value.replace(0, std::min(prefix.size(), value.size()), prefix.substr(0, value.size()));
  return value;
}

auto Path(std::string_view name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tinydb_bench_" + std::string(name) + '_' + std::to_string(::getpid()));
}

void RemoveTiny(const std::filesystem::path &path) {
  auto ignored = std::error_code{};
  std::filesystem::remove(path, ignored);
  const auto wal_name = path.filename().string() + "-wal";
  for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), ignored)) {
    if (entry.path().filename().string().starts_with(wal_name)) {
      std::filesystem::remove(entry.path(), ignored);
    }
  }
}

auto BytesAt(const std::filesystem::path &path) -> std::uint64_t {
  auto ignored = std::error_code{};
  if (!std::filesystem::exists(path, ignored)) {
    return 0;
  }
  if (std::filesystem::is_regular_file(path, ignored)) {
    return std::filesystem::file_size(path, ignored);
  }
  auto bytes = std::uint64_t{0};
  for (const auto &entry : std::filesystem::recursive_directory_iterator(path, ignored)) {
    if (entry.is_regular_file(ignored)) {
      bytes += entry.file_size(ignored);
    }
  }
  return bytes;
}

auto LevelWalBytes(const std::filesystem::path &path) -> std::uint64_t {
  auto bytes = std::uint64_t{0};
  auto ignored = std::error_code{};
  for (const auto &entry : std::filesystem::directory_iterator(path, ignored)) {
    if (entry.is_regular_file(ignored) && entry.path().extension() == ".log") {
      bytes += entry.file_size(ignored);
    }
  }
  return bytes;
}

auto TinyBytes(const std::filesystem::path &path) -> std::uint64_t {
  auto bytes = BytesAt(path);
  const auto wal_name = path.filename().string() + "-wal";
  auto ignored = std::error_code{};
  for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), ignored)) {
    if (entry.path().filename().string().starts_with(wal_name)) {
      bytes += entry.file_size(ignored);
    }
  }
  return bytes;
}

auto TinyOptions() -> tinydb::Options {
  auto options = tinydb::Options{};
  options.page_cache_bytes = 256U << 10U;
  options.wal_segment_bytes = 64U << 20U;
  options.checkpoint.wal_trigger_bytes = std::numeric_limits<std::uint64_t>::max();
  options.checkpoint.dirty_trigger_bytes = std::numeric_limits<std::size_t>::max();
  options.checkpoint.hard_wal_bytes = std::numeric_limits<std::uint64_t>::max();
  options.checkpoint.hard_dirty_bytes = std::numeric_limits<std::size_t>::max();
  options.checkpoint.maximum_age = std::chrono::hours(24);
  return options;
}

auto LevelOptions(leveldb::Cache *cache) -> leveldb::Options {
  auto options = leveldb::Options{};
  options.create_if_missing = true;
  options.compression = leveldb::kNoCompression;
  options.block_cache = cache;
  return options;
}

auto Percentile(std::vector<double> samples, double percentile) -> double {
  if (samples.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::ranges::sort(samples);
  const auto rank = static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(samples.size())));
  return samples[std::max<std::size_t>(rank, 1U) - 1U];
}

void SetLatency(Metrics &metrics, const std::vector<double> &samples) {
  metrics.p50_us = Percentile(samples, 0.50);
  metrics.p95_us = Percentile(samples, 0.95);
  metrics.p99_us = Percentile(samples, 0.99);
}

void FillTiny(tinydb::Database &database, std::size_t rows, std::size_t value_bytes) {
  constexpr auto rows_per_transaction = std::size_t{64};
  for (std::size_t first = 0; first < rows; first += rows_per_transaction) {
    auto write = Take(database.BeginWrite(), "TinyDB BeginWrite");
    for (std::size_t row = first; row < std::min(first + rows_per_transaction, rows); ++row) {
      Check(write.Put(Key(row), Value(row, value_bytes)), "TinyDB Put");
    }
    (void)Take(std::move(write).Commit(), "TinyDB Commit");
  }
}

void FillLevel(leveldb::DB &database, std::size_t rows, std::size_t value_bytes) {
  auto batch = leveldb::WriteBatch{};
  auto write_options = leveldb::WriteOptions{};
  write_options.sync = true;
  for (std::size_t row = 0; row < rows; ++row) {
    batch.Put(Key(row), Value(row, value_bytes));
    if ((row + 1U) % 64U == 0U || row + 1U == rows) {
      Check(database.Write(write_options, &batch), "LevelDB Write");
      batch.Clear();
    }
  }
}

auto TinyTransactions(const Config &config) -> Metrics {
  const auto path = Path("tiny_transactions.db");
  RemoveTiny(path);
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB Open");
  auto samples = std::vector<double>{};
  samples.reserve(config.transactions);
  auto app_bytes = std::uint64_t{0};

  const auto started = Clock::now();
  for (std::size_t transaction = 0; transaction < config.transactions; ++transaction) {
    auto write = Take(database.BeginWrite(), "TinyDB BeginWrite");
    for (std::size_t item = 0; item < config.batch; ++item) {
      const auto row = transaction * config.batch + item;
      const auto key = Key(row);
      const auto value = Value(row, config.value_bytes);
      app_bytes += key.size() + value.size();
      Check(write.Put(key, value), "TinyDB Put");
    }
    const auto commit_started = Clock::now();
    (void)Take(std::move(write).Commit(), "TinyDB Commit");
    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - commit_started).count());
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto stats = Take(database.Stats(), "TinyDB Stats");

  auto metrics = Metrics{.engine = "tinydb",
                         .workload = "transactions_sync",
                         .operations = config.transactions * config.batch,
                         .seconds = seconds,
                         .app_bytes = app_bytes,
                         .wal_bytes = stats.wal_bytes,
                         .file_bytes = TinyBytes(path)};
  metrics.wal_bytes_per_app_byte = static_cast<double>(metrics.wal_bytes) / static_cast<double>(app_bytes);
  SetLatency(metrics, samples);
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

auto LevelTransactions(const Config &config) -> Metrics {
  const auto path = Path("level_transactions");
  std::filesystem::remove_all(path);
  auto cache = std::unique_ptr<leveldb::Cache>{leveldb::NewLRUCache(256U << 10U)};
  auto *raw = static_cast<leveldb::DB *>(nullptr);
  Check(leveldb::DB::Open(LevelOptions(cache.get()), path.string(), &raw), "LevelDB Open");
  auto database = std::unique_ptr<leveldb::DB>{raw};
  auto options = leveldb::WriteOptions{};
  options.sync = true;
  auto samples = std::vector<double>{};
  samples.reserve(config.transactions);
  auto app_bytes = std::uint64_t{0};

  const auto started = Clock::now();
  for (std::size_t transaction = 0; transaction < config.transactions; ++transaction) {
    auto batch = leveldb::WriteBatch{};
    for (std::size_t item = 0; item < config.batch; ++item) {
      const auto row = transaction * config.batch + item;
      const auto key = Key(row);
      const auto value = Value(row, config.value_bytes);
      app_bytes += key.size() + value.size();
      batch.Put(key, value);
    }
    const auto commit_started = Clock::now();
    Check(database->Write(options, &batch), "LevelDB Write");
    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - commit_started).count());
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();

  auto metrics = Metrics{.engine = "leveldb",
                         .workload = "transactions_sync",
                         .operations = config.transactions * config.batch,
                         .seconds = seconds,
                         .app_bytes = app_bytes,
                         .wal_bytes = LevelWalBytes(path),
                         .file_bytes = BytesAt(path)};
  metrics.wal_bytes_per_app_byte = static_cast<double>(metrics.wal_bytes) / static_cast<double>(app_bytes);
  SetLatency(metrics, samples);
  database.reset();
  cache.reset();
  std::filesystem::remove_all(path);
  return metrics;
}

auto TinyPointReads(const Config &config) -> Metrics {
  const auto path = Path("tiny_reads.db");
  RemoveTiny(path);
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB Open");
  FillTiny(database, config.rows, config.value_bytes);
  Check(database.Checkpoint(), "TinyDB Checkpoint");
  for (std::size_t row = 0; row < config.rows; ++row) {
    (void)Take(database.Get(Key(row)), "TinyDB warm read");
  }
  const auto before = Take(database.Stats(), "TinyDB Stats");
  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations.store(true, std::memory_order_relaxed);
  const auto started = Clock::now();
  for (std::size_t row = 0; row < config.rows; ++row) {
    const auto value = Take(database.Get(Key(row)), "TinyDB Get");
    if (!value || value->size() != config.value_bytes) {
      Fail("TinyDB point-read result mismatch");
    }
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  count_allocations.store(false, std::memory_order_relaxed);
  const auto allocations = allocation_count.load(std::memory_order_relaxed);
  const auto after = Take(database.Stats(), "TinyDB Stats");
  const auto hits = after.cache_hits - before.cache_hits;
  const auto misses = after.cache_misses - before.cache_misses;

  auto metrics = Metrics{.engine = "tinydb",
                         .workload = "point_read_hot",
                         .operations = config.rows,
                         .seconds = seconds,
                         .file_bytes = TinyBytes(path)};
  metrics.allocations_per_operation = static_cast<double>(allocations) / static_cast<double>(config.rows);
  metrics.cache_hit_rate = static_cast<double>(hits) / static_cast<double>(hits + misses);
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

auto LevelPointReads(const Config &config) -> Metrics {
  const auto path = Path("level_reads");
  std::filesystem::remove_all(path);
  auto cache = std::unique_ptr<leveldb::Cache>{leveldb::NewLRUCache(256U << 10U)};
  auto *raw = static_cast<leveldb::DB *>(nullptr);
  Check(leveldb::DB::Open(LevelOptions(cache.get()), path.string(), &raw), "LevelDB Open");
  auto database = std::unique_ptr<leveldb::DB>{raw};
  FillLevel(*database, config.rows, config.value_bytes);
  auto options = leveldb::ReadOptions{};
  for (std::size_t row = 0; row < config.rows; ++row) {
    auto value = std::string{};
    Check(database->Get(options, Key(row), &value), "LevelDB warm read");
  }

  allocation_count.store(0, std::memory_order_relaxed);
  count_allocations.store(true, std::memory_order_relaxed);
  const auto started = Clock::now();
  for (std::size_t row = 0; row < config.rows; ++row) {
    auto value = std::string{};
    Check(database->Get(options, Key(row), &value), "LevelDB Get");
    if (value.size() != config.value_bytes) {
      Fail("LevelDB point-read result mismatch");
    }
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  count_allocations.store(false, std::memory_order_relaxed);
  const auto allocations = allocation_count.load(std::memory_order_relaxed);

  auto metrics = Metrics{.engine = "leveldb",
                         .workload = "point_read_hot",
                         .operations = config.rows,
                         .seconds = seconds,
                         .file_bytes = BytesAt(path)};
  metrics.allocations_per_operation = static_cast<double>(allocations) / static_cast<double>(config.rows);
  database.reset();
  cache.reset();
  std::filesystem::remove_all(path);
  return metrics;
}

auto TinyCursor(const Config &config) -> Metrics {
  const auto path = Path("tiny_cursor.db");
  RemoveTiny(path);
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB Open");
  FillTiny(database, config.rows, config.value_bytes);
  Check(database.Checkpoint(), "TinyDB Checkpoint");

  auto rows = std::size_t{0};
  auto seconds = 0.0;
  {
    const auto started = Clock::now();
    auto read = Take(database.BeginRead(), "TinyDB BeginRead");
    auto cursor = Take(read.Scan(), "TinyDB Scan");
    while (cursor.Valid()) {
      rows += 1;
      Check(cursor.Next(), "TinyDB Next");
    }
    seconds = std::chrono::duration<double>(Clock::now() - started).count();
  }
  if (rows != config.rows) {
    Fail("TinyDB cursor row count mismatch");
  }
  auto metrics = Metrics{
      .engine = "tinydb", .workload = "cursor", .operations = rows, .seconds = seconds, .file_bytes = TinyBytes(path)};
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

auto LevelCursor(const Config &config) -> Metrics {
  const auto path = Path("level_cursor");
  std::filesystem::remove_all(path);
  auto cache = std::unique_ptr<leveldb::Cache>{leveldb::NewLRUCache(256U << 10U)};
  auto *raw = static_cast<leveldb::DB *>(nullptr);
  Check(leveldb::DB::Open(LevelOptions(cache.get()), path.string(), &raw), "LevelDB Open");
  auto database = std::unique_ptr<leveldb::DB>{raw};
  FillLevel(*database, config.rows, config.value_bytes);

  const auto started = Clock::now();
  auto cursor = std::unique_ptr<leveldb::Iterator>{database->NewIterator(leveldb::ReadOptions{})};
  auto rows = std::size_t{0};
  for (cursor->SeekToFirst(); cursor->Valid(); cursor->Next()) {
    rows += 1;
  }
  Check(cursor->status(), "LevelDB Iterator");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  if (rows != config.rows) {
    Fail("LevelDB cursor row count mismatch");
  }
  auto metrics = Metrics{
      .engine = "leveldb", .workload = "cursor", .operations = rows, .seconds = seconds, .file_bytes = BytesAt(path)};
  cursor.reset();
  database.reset();
  cache.reset();
  std::filesystem::remove_all(path);
  return metrics;
}

auto TinyCheckpoint(const Config &config) -> Metrics {
  const auto path = Path("tiny_checkpoint.db");
  RemoveTiny(path);
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB Open");
  FillTiny(database, config.rows, config.value_bytes);
  const auto before = Take(database.Stats(), "TinyDB Stats");
  const auto started = Clock::now();
  Check(database.Checkpoint(), "TinyDB Checkpoint");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();

  auto metrics = Metrics{.engine = "tinydb",
                         .workload = "checkpoint",
                         .operations = before.dirty_pages,
                         .seconds = seconds,
                         .wal_bytes = before.wal_bytes,
                         .file_bytes = TinyBytes(path)};
  metrics.checkpoint_ms = seconds * 1'000.0;
  metrics.checkpoint_mib_per_second = (static_cast<double>(before.dirty_bytes) / (1024.0 * 1024.0)) / seconds;
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

auto TinyReaderWait(const Config &config) -> Metrics {
  const auto path = Path("tiny_reader_wait.db");
  RemoveTiny(path);
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB Open");
  FillTiny(database, 1, config.value_bytes);
  auto reader = std::make_unique<tinydb::ReadTransaction>(Take(database.BeginRead(), "TinyDB BeginRead"));
  auto commit_status = tinydb::Status{};
  auto writer = std::thread{[&] {
    auto write = Take(database.BeginWrite(), "TinyDB BeginWrite");
    Check(write.Put("wait", "value"), "TinyDB Put");
    const auto committed = std::move(write).Commit();
    if (!committed) {
      commit_status = committed.error();
    }
  }};

  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (!Take(database.Stats(), "TinyDB Stats").publication_pending && Clock::now() < deadline) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reader.reset();
  writer.join();
  Check(commit_status, "TinyDB waiting Commit");
  const auto stats = Take(database.Stats(), "TinyDB Stats");

  auto metrics = Metrics{.engine = "tinydb",
                         .workload = "reader_publication_wait",
                         .operations = 1,
                         .seconds = std::chrono::duration<double>(stats.last_publication_wait).count(),
                         .file_bytes = TinyBytes(path)};
  metrics.p50_us = std::chrono::duration<double, std::micro>(stats.last_publication_wait).count();
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

void ChildTinyWrites(const std::filesystem::path &path, const Config &config) {
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB child Open");
  FillTiny(database, config.rows, config.value_bytes);
  ::_exit(0);
}

void ChildLevelWrites(const std::filesystem::path &path, const Config &config) {
  auto cache = std::unique_ptr<leveldb::Cache>{leveldb::NewLRUCache(256U << 10U)};
  auto *database = static_cast<leveldb::DB *>(nullptr);
  Check(leveldb::DB::Open(LevelOptions(cache.get()), path.string(), &database), "LevelDB child Open");
  FillLevel(*database, config.rows, config.value_bytes);
  ::_exit(0);
}

void Wait(pid_t child) {
  auto status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    Fail("benchmark writer child failed");
  }
}

auto TinyRecovery(const Config &config) -> Metrics {
  const auto path = Path("tiny_recovery.db");
  RemoveTiny(path);
  const auto child = ::fork();
  if (child == 0) {
    ChildTinyWrites(path, config);
  }
  if (child < 0) {
    Fail("fork failed");
  }
  Wait(child);
  const auto started = Clock::now();
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB recovery Open");
  const auto elapsed = Clock::now() - started;
  if (!Take(database.Get(Key(config.rows - 1U)), "TinyDB recovery Get")) {
    Fail("TinyDB recovery lost acknowledged data");
  }
  auto metrics = Metrics{.engine = "tinydb",
                         .workload = "recovery",
                         .operations = config.rows,
                         .seconds = std::chrono::duration<double>(elapsed).count(),
                         .file_bytes = TinyBytes(path)};
  metrics.recovery_ms = std::chrono::duration<double, std::milli>(elapsed).count();
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

auto LevelRecovery(const Config &config) -> Metrics {
  const auto path = Path("level_recovery");
  std::filesystem::remove_all(path);
  const auto child = ::fork();
  if (child == 0) {
    ChildLevelWrites(path, config);
  }
  if (child < 0) {
    Fail("fork failed");
  }
  Wait(child);
  auto cache = std::unique_ptr<leveldb::Cache>{leveldb::NewLRUCache(256U << 10U)};
  auto *raw = static_cast<leveldb::DB *>(nullptr);
  const auto started = Clock::now();
  Check(leveldb::DB::Open(LevelOptions(cache.get()), path.string(), &raw), "LevelDB recovery Open");
  const auto elapsed = Clock::now() - started;
  auto database = std::unique_ptr<leveldb::DB>{raw};
  auto value = std::string{};
  Check(database->Get(leveldb::ReadOptions{}, Key(config.rows - 1U), &value), "LevelDB recovery Get");
  auto metrics = Metrics{.engine = "leveldb",
                         .workload = "recovery",
                         .operations = config.rows,
                         .seconds = std::chrono::duration<double>(elapsed).count(),
                         .file_bytes = BytesAt(path)};
  metrics.recovery_ms = std::chrono::duration<double, std::milli>(elapsed).count();
  database.reset();
  cache.reset();
  std::filesystem::remove_all(path);
  return metrics;
}

auto TinyChurn(const Config &config) -> Metrics {
  const auto path = Path("tiny_churn.db");
  RemoveTiny(path);
  auto database = Take(tinydb::Database::Open(path, TinyOptions()), "TinyDB Open");
  const auto started = Clock::now();
  for (std::size_t round = 0; round < config.churn_rounds; ++round) {
    FillTiny(database, config.rows, config.value_bytes);
    auto write = Take(database.BeginWrite(), "TinyDB BeginWrite");
    for (std::size_t row = 0; row < config.rows; ++row) {
      Check(write.Delete(Key(row)), "TinyDB Delete");
    }
    (void)Take(std::move(write).Commit(), "TinyDB Commit");
    Check(database.Checkpoint(), "TinyDB Checkpoint");
  }
  FillTiny(database, config.rows, config.value_bytes);
  Check(database.Checkpoint(), "TinyDB Checkpoint");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto operations = config.rows * (2U * config.churn_rounds + 1U);
  auto metrics = Metrics{.engine = "tinydb",
                         .workload = "churn_reuse",
                         .operations = operations,
                         .seconds = seconds,
                         .file_bytes = TinyBytes(path)};
  Check(database.Close(), "TinyDB Close");
  RemoveTiny(path);
  return metrics;
}

auto LevelChurn(const Config &config) -> Metrics {
  const auto path = Path("level_churn");
  std::filesystem::remove_all(path);
  auto cache = std::unique_ptr<leveldb::Cache>{leveldb::NewLRUCache(256U << 10U)};
  auto *raw = static_cast<leveldb::DB *>(nullptr);
  Check(leveldb::DB::Open(LevelOptions(cache.get()), path.string(), &raw), "LevelDB Open");
  auto database = std::unique_ptr<leveldb::DB>{raw};
  auto options = leveldb::WriteOptions{};
  options.sync = true;
  const auto started = Clock::now();
  for (std::size_t round = 0; round < config.churn_rounds; ++round) {
    FillLevel(*database, config.rows, config.value_bytes);
    auto batch = leveldb::WriteBatch{};
    for (std::size_t row = 0; row < config.rows; ++row) {
      batch.Delete(Key(row));
    }
    Check(database->Write(options, &batch), "LevelDB Delete batch");
  }
  FillLevel(*database, config.rows, config.value_bytes);
  database->CompactRange(nullptr, nullptr);
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto operations = config.rows * (2U * config.churn_rounds + 1U);
  auto metrics = Metrics{.engine = "leveldb",
                         .workload = "churn_reuse",
                         .operations = operations,
                         .seconds = seconds,
                         .file_bytes = BytesAt(path)};
  database.reset();
  cache.reset();
  std::filesystem::remove_all(path);
  return metrics;
}

auto ParseSize(std::string_view text, std::string_view flag) -> std::size_t {
  auto value = std::size_t{0};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0) {
    Fail(std::string("invalid ") + std::string(flag));
  }
  return value;
}

auto Parse(int argc, char **argv) -> Config {
  auto config = Config{};
  for (auto index = 1; index < argc; ++index) {
    const auto flag = std::string_view{argv[index]};
    if (index + 1 >= argc) {
      Fail("missing benchmark option value");
    }
    const auto value = ParseSize(argv[++index], flag);
    if (flag == "--rows")
      config.rows = value;
    else if (flag == "--transactions")
      config.transactions = value;
    else if (flag == "--batch")
      config.batch = value;
    else if (flag == "--value-bytes")
      config.value_bytes = value;
    else if (flag == "--churn-rounds")
      config.churn_rounds = value;
    else
      Fail(std::string("unknown benchmark option: ") + std::string(flag));
  }
  return config;
}

void Print(const Metrics &metrics) {
  const auto rate = static_cast<double>(metrics.operations) / metrics.seconds;
  std::printf("%s,%s,%llu,%.9f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%.6f,%.3f,%.3f,%.6f,%.6f,%.3f,%llu\n",
              metrics.engine.c_str(), metrics.workload.c_str(), static_cast<unsigned long long>(metrics.operations),
              metrics.seconds, rate, metrics.p50_us, metrics.p95_us, metrics.p99_us,
              static_cast<unsigned long long>(metrics.app_bytes), static_cast<unsigned long long>(metrics.wal_bytes),
              metrics.wal_bytes_per_app_byte, metrics.checkpoint_ms, metrics.checkpoint_mib_per_second,
              metrics.allocations_per_operation, metrics.cache_hit_rate, metrics.recovery_ms,
              static_cast<unsigned long long>(metrics.file_bytes));
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const auto config = Parse(argc, argv);
  std::puts(
      "engine,workload,operations,seconds,ops_per_second,p50_us,p95_us,p99_us,app_bytes,wal_bytes,"
      "wal_bytes_per_app_byte,checkpoint_ms,checkpoint_mib_per_second,allocations_per_operation,"
      "cache_hit_rate,recovery_ms,file_bytes");
  Print(TinyTransactions(config));
  Print(LevelTransactions(config));
  Print(TinyPointReads(config));
  Print(LevelPointReads(config));
  Print(TinyCursor(config));
  Print(LevelCursor(config));
  Print(TinyCheckpoint(config));
  Print(TinyReaderWait(config));
  Print(TinyRecovery(config));
  Print(LevelRecovery(config));
  Print(TinyChurn(config));
  Print(LevelChurn(config));
  return 0;
}
