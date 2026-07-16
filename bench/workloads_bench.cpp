// Build and run:
//   cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release -DTINYDB_BUILD_BENCHMARKS=ON
//   cmake --build cmake-build-release --target TinyDB_workloads_bench
//   ./cmake-build-release/TinyDB_workloads_bench
//
// The two "cold" levels: PointReadCold misses the buffer pool but the OS
// page cache still absorbs the pread, so it isolates the engine's pool-miss
// path. PointReadColdDisk additionally evicts the file from the page cache
// (posix_fadvise) between read batches, so misses pay for real device I/O.
// The database file must live on a disk-backed filesystem for that to mean
// anything — on a tmpfs /tmp the "device" is RAM and eviction is a no-op.

#include <benchmark/benchmark.h>
#include <tinydb/check.h>
#include <tinydb/storage_engine.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

// Fixed payload size. With 12-byte keys an entry costs ~120 bytes on a page,
// so a fully packed leaf holds around 33 rows.
constexpr std::size_t VALUE_BYTES = 100;

// PointReadColdDisk evicts the OS page cache after this many reads.
constexpr std::int64_t READS_PER_EVICTION = 100;

auto BenchPath(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("tinydb_bench_" + name + "_" + std::to_string(::getpid()) + ".db");
}

auto RowKey(std::int64_t row) -> std::string {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "row-%08lld", static_cast<long long>(row));
  return std::string{buffer};
}

auto RowValue(std::int64_t row) -> std::string {
  auto value = std::string(VALUE_BYTES, '\0');
  for (std::size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<char>('a' + (static_cast<std::size_t>(row) + i) % 26);
  }
  return value;
}

// Deterministic shuffle: every run and every benchmark sees the same
// "random" order, so results stay comparable across code changes.
auto ShuffledRows(std::int64_t rows) -> std::vector<std::int64_t> {
  auto order = std::vector<std::int64_t>(static_cast<std::size_t>(rows));
  std::iota(order.begin(), order.end(), std::int64_t{0});
  auto rng = std::mt19937{42};
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

struct Row {
  std::string key;
  std::string value;
};

auto SequentialRowSet(std::int64_t rows) -> std::vector<Row> {
  auto result = std::vector<Row>{};
  result.reserve(static_cast<std::size_t>(rows));
  for (std::int64_t row = 0; row < rows; ++row) {
    result.push_back({RowKey(row), RowValue(row)});
  }
  return result;
}

auto ShuffledRowSet(std::int64_t rows) -> std::vector<Row> {
  auto result = std::vector<Row>{};
  result.reserve(static_cast<std::size_t>(rows));
  for (const auto row : ShuffledRows(rows)) {
    result.push_back({RowKey(row), RowValue(row)});
  }
  return result;
}

auto ShuffledKeys(std::int64_t rows) -> std::vector<std::string> {
  auto keys = std::vector<std::string>{};
  keys.reserve(static_cast<std::size_t>(rows));
  for (const auto row : ShuffledRows(rows)) {
    keys.push_back(RowKey(row));
  }
  return keys;
}

// The read benchmarks share sequentially loaded databases, built once per
// process: rebuilding 100k rows on every benchmark invocation would dwarf
// the reads being measured. Files are removed when the process exits.
class PrebuiltDatabase {
 public:
  explicit PrebuiltDatabase(std::int64_t rows) : path_(BenchPath("prebuilt_" + std::to_string(rows))) {
    std::filesystem::remove(path_);
    auto engine = tinydb::StorageEngine::Open(path_).value();
    for (std::int64_t row = 0; row < rows; ++row) {
      TINYDB_CHECK(engine.Put(RowKey(row), RowValue(row)).Ok(), "prebuild Put failed");
    }
    TINYDB_CHECK(engine.Close().Ok(), "prebuild Close failed");
  }

  PrebuiltDatabase(const PrebuiltDatabase &) = delete;
  auto operator=(const PrebuiltDatabase &) -> PrebuiltDatabase & = delete;
  PrebuiltDatabase(PrebuiltDatabase &&) = delete;
  auto operator=(PrebuiltDatabase &&) -> PrebuiltDatabase & = delete;

  ~PrebuiltDatabase() {
    auto ignored = std::error_code{};
    std::filesystem::remove(path_, ignored);
  }

  auto Path() const -> const std::filesystem::path & { return path_; }

 private:
  std::filesystem::path path_;
};

auto PrebuiltDb(std::int64_t rows) -> const std::filesystem::path & {
  static auto databases = std::map<std::int64_t, PrebuiltDatabase>{};
  return databases.try_emplace(rows, rows).first->second.Path();
}

// Evicts a file's pages from the OS page cache so the next reads hit the
// storage device. The page cache is per-inode, not per-descriptor, so
// advising through this separate read-only descriptor also evicts what the
// engine's own descriptor would otherwise find cached. Only clean pages are
// dropped, which is fine here: the prebuilt databases are closed and synced.
class PageCacheEvictor {
 public:
  explicit PageCacheEvictor(const std::filesystem::path &path) : fd_(::open(path.c_str(), O_RDONLY)) {
    TINYDB_CHECK(fd_ >= 0, "opening the database for cache eviction failed");
  }

  PageCacheEvictor(const PageCacheEvictor &) = delete;
  auto operator=(const PageCacheEvictor &) -> PageCacheEvictor & = delete;
  PageCacheEvictor(PageCacheEvictor &&) = delete;
  auto operator=(PageCacheEvictor &&) -> PageCacheEvictor & = delete;

  ~PageCacheEvictor() { ::close(fd_); }

  auto Evict() const -> void {
    TINYDB_CHECK(::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED) == 0, "posix_fadvise failed");
  }

 private:
  int fd_;
};

// Shared body of the two insert benchmarks: each iteration bulk-loads a
// fresh database, timing only the Puts. Open and the closing flush + fsync
// stay outside the clock; per-operation durability is the WAL's job later.
void InsertWorkload(benchmark::State &state, const std::vector<Row> &rows, const std::string &label) {
  const auto path = BenchPath(label);
  while (state.KeepRunning()) {
    std::filesystem::remove(path);
    auto engine = tinydb::StorageEngine::Open(path).value();

    const auto start = std::chrono::steady_clock::now();
    for (const auto &row : rows) {
      benchmark::DoNotOptimize(engine.Put(row.key, row.value));
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    state.SetIterationTime(seconds.count());

    TINYDB_CHECK(engine.Close().Ok(), "Close failed");
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(rows.size()));
  state.counters["db_MiB"] = static_cast<double>(std::filesystem::file_size(path)) / (1024.0 * 1024.0);
  std::filesystem::remove(path);
}

void SequentialInsert(benchmark::State &state) {
  // Ascending keys always land on the rightmost leaf, where the tail-heavy
  // split keeps finished leaves nearly full: the friendliest write pattern,
  // and the smallest file. Compare db_MiB with RandomInsert's.
  InsertWorkload(state, SequentialRowSet(state.range(0)), "sequential_insert");
}
BENCHMARK(SequentialInsert)->Arg(10'000)->UseManualTime()->Unit(benchmark::kMillisecond);

void RandomInsert(benchmark::State &state) {
  // Uniformly random keys hit arbitrary leaves: splits fall down the middle
  // and leaves settle near half full, so expect a lower rate and a larger
  // file than SequentialInsert.
  InsertWorkload(state, ShuffledRowSet(state.range(0)), "random_insert");
}
BENCHMARK(RandomInsert)->Arg(10'000)->UseManualTime()->Unit(benchmark::kMillisecond);

void PointReadHot(benchmark::State &state) {
  // At 1,000 rows the whole tree is ~35 pages, comfortably inside the
  // 64-frame buffer pool: after warmup every Get is served from memory.
  const auto keys = ShuffledKeys(state.range(0));
  auto engine = tinydb::StorageEngine::Open(PrebuiltDb(state.range(0))).value();

  for (const auto &key : keys) {  // warm the pool
    auto value = engine.Get(key);
    benchmark::DoNotOptimize(value);
  }

  std::size_t next = 0;
  while (state.KeepRunning()) {
    auto value = engine.Get(keys[next]);
    benchmark::DoNotOptimize(value);
    next = (next + 1) % keys.size();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(PointReadHot)->Arg(1'000);

void PointReadCold(benchmark::State &state) {
  // At 100,000 rows the tree is ~3,000 leaf pages against 64 pool frames,
  // so with a uniform random key stream nearly every Get evicts a frame and
  // reads a page. Per the header caveat, the OS page cache absorbs the
  // pread: this measures the pool-miss path, not disk latency.
  const auto keys = ShuffledKeys(state.range(0));
  auto engine = tinydb::StorageEngine::Open(PrebuiltDb(state.range(0))).value();

  std::size_t next = 0;
  while (state.KeepRunning()) {
    auto value = engine.Get(keys[next]);
    benchmark::DoNotOptimize(value);
    next = (next + 1) % keys.size();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(PointReadCold)->Arg(100'000);

void PointReadColdDisk(benchmark::State &state) {
  // Like PointReadCold, but the OS page cache is evicted between read
  // batches, so a buffer-pool miss costs a real device read. The batch is
  // small relative to the ~3,000 leaves (a read within a batch revisits an
  // already-warmed leaf ~2% of the time), and eviction runs off the clock.
  const auto keys = ShuffledKeys(state.range(0));
  const auto &path = PrebuiltDb(state.range(0));
  auto engine = tinydb::StorageEngine::Open(path).value();
  const auto evictor = PageCacheEvictor{path};

  std::size_t next = 0;
  while (state.KeepRunningBatch(READS_PER_EVICTION)) {
    state.PauseTiming();
    evictor.Evict();
    state.ResumeTiming();

    for (std::int64_t i = 0; i < READS_PER_EVICTION; ++i) {
      auto value = engine.Get(keys[next]);
      benchmark::DoNotOptimize(value);
      next = (next + 1) % keys.size();
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(PointReadColdDisk)->Arg(100'000)->Unit(benchmark::kMicrosecond);

void RangeScan(benchmark::State &state) {
  // Full-table scan following the leaf sibling chain. Bytes/sec counts the
  // key and copied-value payload returned. The cursor retains one leaf rather
  // than allocating storage proportional to the full result set.
  const auto rows = static_cast<std::int64_t>(state.range(0));
  auto engine = tinydb::StorageEngine::Open(PrebuiltDb(rows)).value();

  std::int64_t payload_bytes = 0;
  auto sample_rows = std::int64_t{0};
  {
    auto transaction = engine.BeginRead().value();
    auto cursor = transaction.Scan(tinydb::KeyRange::All()).value();
    while (cursor.Valid()) {
      const auto value = cursor.CopyValue().value();
      payload_bytes += std::ssize(cursor.Key()) + std::ssize(value);
      ++sample_rows;
      TINYDB_CHECK(cursor.Next().Ok(), "sample cursor failed");
    }
  }
  TINYDB_CHECK(sample_rows == rows, "prebuilt database is missing rows");

  while (state.KeepRunning()) {
    auto transaction = engine.BeginRead().value();
    auto cursor = transaction.Scan(tinydb::KeyRange::All()).value();
    while (cursor.Valid()) {
      auto value = cursor.CopyValue();
      benchmark::DoNotOptimize(value);
      TINYDB_CHECK(cursor.Next().Ok(), "range cursor failed");
    }
  }

  state.SetItemsProcessed(state.iterations() * rows);
  state.SetBytesProcessed(state.iterations() * payload_bytes);
}
BENCHMARK(RangeScan)->Arg(100'000)->Unit(benchmark::kMillisecond);

void Churn(benchmark::State &state) {
  // Steady-state churn: each iteration inserts a batch and removes it
  // again, so the tree repeatedly splits on the way up and merges back
  // down, cycling pages through the free list. The db_KiB counter staying
  // small — instead of growing with iterations — is the free list working.
  const auto rows = SequentialRowSet(state.range(0));
  const auto path = BenchPath("churn");
  std::filesystem::remove(path);
  auto engine = tinydb::StorageEngine::Open(path).value();

  while (state.KeepRunning()) {
    for (const auto &row : rows) {
      benchmark::DoNotOptimize(engine.Put(row.key, row.value));
    }
    for (const auto &row : rows) {
      benchmark::DoNotOptimize(engine.Remove(row.key));
    }
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(rows.size()) * 2);
  TINYDB_CHECK(engine.Close().Ok(), "Close failed");
  state.counters["db_KiB"] = static_cast<double>(std::filesystem::file_size(path)) / 1024.0;
  std::filesystem::remove(path);
}
BENCHMARK(Churn)->Arg(200)->Unit(benchmark::kMillisecond);

}  // namespace
