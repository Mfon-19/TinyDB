#include <tinydb/database.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/*
** TINYDB MEASUREMENT HARNESS
**
** This program measures one engine and makes no cross-engine performance
** claim. Each workload has four phases:
**
**     construct deterministic fixture
**     run and discard warmup trials
**     run separate measured trials
**     validate results and summarize the sample distribution
**
** Fixture construction, checkpoint preparation, and correctness validation
** are outside timed intervals unless they are the operation named by the
** workload. CPU-bound trials repeat whole passes until minimum_trial_ms has
** elapsed, which keeps clock overhead below the operation being measured.
** Durability trials perform a fixed number of synchronous commits and retain
** every commit latency as a sample.
**
** stdout is stable CSV. Configuration goes to stderr so redirecting stdout
** produces a data file without machine-specific prose. A fixed seed controls
** access order; every sample count and variability statistic is present in
** the output instead of being hidden behind a single best run.
*/
struct Config final {
  std::size_t rows{5'000};
  std::size_t transactions{64};
  std::size_t batch{16};
  std::size_t value_bytes{128};
  std::size_t trials{7};
  std::size_t warmups{2};
  std::size_t minimum_trial_ms{250};
  std::size_t churn_rounds{3};
  std::size_t cache_bytes{16U << 20U};
  std::uint64_t seed{0x54494E594442ULL};
  std::string workload{"all"};
};

struct Dataset final {
  std::vector<std::string> keys;
  std::vector<std::string> first_values;
  std::vector<std::string> second_values;
  std::uint64_t logical_bytes{0};
};

struct SampleSet final {
  std::string benchmark;
  std::string metric;
  std::string unit;
  std::vector<double> samples;
};

struct WriteObservation final {
  double operations_per_second{0};
  double wal_amplification{0};
  double persistent_bytes{0};
  std::vector<double> commit_microseconds;
};

struct PointObservation final {
  double operations_per_second{0};
  double nanoseconds_per_operation{0};
  double cache_hit_rate{0};
};

struct CheckpointObservation final {
  double milliseconds{0};
  double mebibytes_per_second{0};
  double database_bytes{0};
};

struct RecoveryObservation final {
  double milliseconds{0};
  double mebibytes_per_second{0};
};

struct ChurnObservation final {
  double operations_per_second{0};
  double file_amplification{0};
  double database_bytes{0};
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

template <typename T>
auto Take(tinydb::Result<T> result, std::string_view operation) -> T {
  if (!result) {
    Fail(std::string(operation) + ": " + result.error().ToString());
  }
  return std::move(*result);
}

auto CheckedMultiply(std::size_t left, std::size_t right, std::string_view description) -> std::size_t {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    Fail(std::string(description) + " is too large");
  }
  return left * right;
}

auto Key(std::size_t row) -> std::string {
  auto digits = std::array<char, 32>{};
  const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), row);
  if (result.ec != std::errc{}) {
    Fail("cannot encode benchmark key");
  }
  const auto digit_count = static_cast<std::size_t>(result.ptr - digits.data());
  auto key = std::string{"key/"};
  if (digit_count < 16U) {
    key.append(16U - digit_count, '0');
  }
  key.append(digits.data(), digit_count);
  return key;
}

auto Value(std::size_t row, std::size_t bytes, std::size_t generation) -> std::string {
  auto value = std::string(bytes, static_cast<char>('a' + (row + generation) % 26U));
  const auto prefix = std::to_string(row) + ':' + std::to_string(generation) + ':';
  value.replace(0, std::min(prefix.size(), value.size()), prefix.substr(0, value.size()));
  return value;
}

auto MakeDataset(std::size_t rows, std::size_t value_bytes) -> Dataset {
  auto data = Dataset{};
  data.keys.reserve(rows);
  data.first_values.reserve(rows);
  data.second_values.reserve(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    data.keys.push_back(Key(row));
    data.first_values.push_back(Value(row, value_bytes, 0));
    data.second_values.push_back(Value(row, value_bytes, 1));
    data.logical_bytes += data.keys.back().size() + data.first_values.back().size();
  }
  return data;
}

auto Path(std::string_view workload) -> std::filesystem::path {
  static auto sequence = std::atomic<std::uint64_t>{0};
  return std::filesystem::temp_directory_path() /
         ("tinydb_bench_" + std::string(workload) + '_' + std::to_string(::getpid()) + '_' +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".db");
}

/* Remove the database and every segmented WAL belonging to its basename.
** Cleanup errors are deliberately ignored after a trial; an operation error
** inside a trial remains fatal and is never converted into a timing sample. */
void RemoveDatabase(const std::filesystem::path &path) {
  auto ignored = std::error_code{};
  std::filesystem::remove(path, ignored);
  const auto wal_prefix = path.filename().string() + "-wal";
  for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), ignored)) {
    if (entry.path().filename().string().starts_with(wal_prefix)) {
      std::filesystem::remove(entry.path(), ignored);
    }
  }
}

auto FileBytes(const std::filesystem::path &path) -> std::uint64_t {
  auto ignored = std::error_code{};
  if (!std::filesystem::exists(path, ignored)) {
    return 0;
  }
  return std::filesystem::file_size(path, ignored);
}

auto PersistentBytes(const std::filesystem::path &path) -> std::uint64_t {
  auto bytes = FileBytes(path);
  auto ignored = std::error_code{};
  const auto wal_prefix = path.filename().string() + "-wal";
  for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), ignored)) {
    if (entry.path().filename().string().starts_with(wal_prefix) && entry.is_regular_file(ignored)) {
      bytes += entry.file_size(ignored);
    }
  }
  return bytes;
}

/* Automatic checkpoint pressure is disabled so an explicit checkpoint is the
** only event moving data-file state during a trial. Synchronous commit
** durability is unchanged. The large WAL segment also removes segment
** rotation as an accidental boundary in short latency samples. */
auto BenchmarkOptions(const Config &config) -> tinydb::Options {
  auto options = tinydb::Options{};
  options.page_cache_bytes = config.cache_bytes;
  options.max_write_transaction_bytes = 32U << 20U;
  options.wal_segment_bytes = 64U << 20U;
  options.checkpoint.wal_trigger_bytes = std::numeric_limits<std::uint64_t>::max();
  options.checkpoint.dirty_trigger_bytes = std::numeric_limits<std::size_t>::max();
  options.checkpoint.hard_wal_bytes = std::numeric_limits<std::uint64_t>::max();
  options.checkpoint.hard_dirty_bytes = std::numeric_limits<std::size_t>::max();
  options.checkpoint.maximum_age = std::chrono::hours(24);
  return options;
}

auto SetupBatchSize(const Config &config) -> std::size_t {
  constexpr auto target_payload = std::size_t{4U << 20U};
  return std::max<std::size_t>(1U, std::min<std::size_t>(256U, target_payload / config.value_bytes));
}

/* Populate or overwrite a fixture in bounded transactions. These commits are
** fixture construction and callers place them outside the timed phase. */
void StoreDataset(tinydb::Database &database, const Dataset &data, bool second, const Config &config) {
  const auto &values = second ? data.second_values : data.first_values;
  const auto batch = SetupBatchSize(config);
  for (std::size_t first = 0; first < data.keys.size(); first += batch) {
    auto write = Take(database.BeginWrite(), "BeginWrite fixture");
    for (std::size_t row = first; row < std::min(first + batch, data.keys.size()); ++row) {
      Check(write.Put(data.keys[row], values[row]), "Put fixture");
    }
    (void)Take(std::move(write).Commit(), "Commit fixture");
  }
}

void DeleteDataset(tinydb::Database &database, const Dataset &data) {
  constexpr auto batch = std::size_t{256};
  for (std::size_t first = 0; first < data.keys.size(); first += batch) {
    auto write = Take(database.BeginWrite(), "BeginWrite delete fixture");
    for (std::size_t row = first; row < std::min(first + batch, data.keys.size()); ++row) {
      Check(write.Delete(data.keys[row]), "Delete fixture");
    }
    (void)Take(std::move(write).Commit(), "Commit delete fixture");
  }
}

auto ValueDigest(std::string_view value) -> std::uint64_t {
  if (value.empty()) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.size()) * 131U + static_cast<unsigned char>(value.front()) * 17U +
         static_cast<unsigned char>(value.back());
}

auto Percentile(const std::vector<double> &sorted, double percentile) -> double {
  if (sorted.empty()) {
    Fail("cannot summarize an empty sample set");
  }
  const auto position = percentile * static_cast<double>(sorted.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const auto fraction = position - static_cast<double>(lower);
  return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

/* Percentiles use linear interpolation over zero-based ranks. Standard
** deviation is the sample deviation (N-1), because trials are observations of
** a larger population of possible runs on this machine. */
void Print(const SampleSet &set) {
  if (set.samples.empty()) {
    Fail("benchmark produced no samples");
  }
  auto sorted = set.samples;
  std::ranges::sort(sorted);
  const auto mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
  auto squared_difference = 0.0;
  for (const auto sample : sorted) {
    const auto difference = sample - mean;
    squared_difference += difference * difference;
  }
  const auto deviation =
      sorted.size() > 1U ? std::sqrt(squared_difference / static_cast<double>(sorted.size() - 1U)) : 0.0;
  std::printf("%s,%s,%s,%zu,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n", set.benchmark.c_str(), set.metric.c_str(),
              set.unit.c_str(), sorted.size(), mean, deviation, sorted.front(), Percentile(sorted, 0.50),
              Percentile(sorted, 0.95), Percentile(sorted, 0.99), sorted.back());
}

void Print(const std::vector<SampleSet> &sets) {
  for (const auto &set : sets) {
    Print(set);
  }
}

auto ObserveWrite(const Config &config, const Dataset &data, bool overwrite,
                  std::string_view name) -> WriteObservation {
  const auto path = Path(name);
  RemoveDatabase(path);
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open write benchmark");
  if (overwrite) {
    StoreDataset(database, data, false, config);
    Check(database.Checkpoint(), "Checkpoint overwrite fixture");
  }

  const auto before = Take(database.Stats(), "Stats before write benchmark");
  auto latencies = std::vector<double>{};
  latencies.reserve(config.transactions);
  const auto started = Clock::now();
  for (std::size_t transaction = 0; transaction < config.transactions; ++transaction) {
    auto write = Take(database.BeginWrite(), "BeginWrite measured");
    for (std::size_t item = 0; item < config.batch; ++item) {
      const auto row = transaction * config.batch + item;
      const auto &value = overwrite ? data.second_values[row] : data.first_values[row];
      Check(write.Put(data.keys[row], value), "Put measured");
    }
    const auto commit_started = Clock::now();
    (void)Take(std::move(write).Commit(), "Commit measured");
    latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - commit_started).count());
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after write benchmark");
  const auto bytes = PersistentBytes(path);
  Check(database.Close(), "Close write benchmark");
  RemoveDatabase(path);

  const auto operations = CheckedMultiply(config.transactions, config.batch, "write operation count");
  const auto wal_delta = after.wal_bytes >= before.wal_bytes ? after.wal_bytes - before.wal_bytes : 0;
  return WriteObservation{
      .operations_per_second = static_cast<double>(operations) / seconds,
      .wal_amplification = static_cast<double>(wal_delta) / static_cast<double>(data.logical_bytes),
      .persistent_bytes = static_cast<double>(bytes),
      .commit_microseconds = std::move(latencies),
  };
}

auto RunWriteBenchmark(const Config &config, bool overwrite) -> std::vector<SampleSet> {
  const auto operations = CheckedMultiply(config.transactions, config.batch, "transactions times batch");
  const auto data = MakeDataset(operations, config.value_bytes);
  const auto name = std::string{overwrite ? "put_overwrite" : "put_insert"};
  for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
    (void)ObserveWrite(config, data, overwrite, name);
  }

  auto throughput = SampleSet{name, "throughput", "operations/second", {}};
  auto latency = SampleSet{name, "commit_latency", "microseconds", {}};
  auto wal = SampleSet{name, "wal_amplification", "bytes/byte", {}};
  auto bytes = SampleSet{name, "persistent_size", "bytes", {}};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    auto observation = ObserveWrite(config, data, overwrite, name);
    throughput.samples.push_back(observation.operations_per_second);
    latency.samples.insert(latency.samples.end(), observation.commit_microseconds.begin(),
                           observation.commit_microseconds.end());
    wal.samples.push_back(observation.wal_amplification);
    bytes.samples.push_back(observation.persistent_bytes);
  }
  return {std::move(throughput), std::move(latency), std::move(wal), std::move(bytes)};
}

auto ExpectedValueDigest(const Dataset &data) -> std::uint64_t {
  auto digest = std::uint64_t{0};
  for (const auto &value : data.first_values) {
    digest += ValueDigest(value);
  }
  return digest;
}

auto ObservePointReads(tinydb::Database &database, const Dataset &data, const std::vector<std::size_t> &order,
                       const Config &config, bool transaction_scoped) -> PointObservation {
  const auto before = Take(database.Stats(), "Stats before point reads");
  const auto target = std::chrono::milliseconds(config.minimum_trial_ms);
  auto operations = std::uint64_t{0};
  auto passes = std::uint64_t{0};
  auto digest = std::uint64_t{0};
  const auto started = Clock::now();
  do {
    if (transaction_scoped) {
      auto read = Take(database.BeginRead(), "BeginRead measured");
      for (const auto row : order) {
        const auto value = Take(read.Get(data.keys[row]), "ReadTransaction Get measured");
        if (!value) {
          Fail("transaction-scoped point read missed a fixture key");
        }
        digest += ValueDigest(*value);
      }
    } else {
      for (const auto row : order) {
        const auto value = Take(database.Get(data.keys[row]), "Database Get measured");
        if (!value) {
          Fail("convenience point read missed a fixture key");
        }
        digest += ValueDigest(*value);
      }
    }
    operations += data.keys.size();
    passes += 1;
  } while (Clock::now() - started < target);
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after point reads");
  if (digest != ExpectedValueDigest(data) * passes) {
    Fail("point-read validation digest mismatch");
  }
  const auto hits = after.cache_hits - before.cache_hits;
  const auto misses = after.cache_misses - before.cache_misses;
  const auto accesses = hits + misses;
  return PointObservation{
      .operations_per_second = static_cast<double>(operations) / seconds,
      .nanoseconds_per_operation = seconds * 1'000'000'000.0 / static_cast<double>(operations),
      .cache_hit_rate = accesses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(accesses),
  };
}

auto RunPointBenchmark(const Config &config, bool transaction_scoped) -> std::vector<SampleSet> {
  const auto name = std::string{transaction_scoped ? "read_transaction_get_hot" : "database_get_hot"};
  const auto path = Path(name);
  RemoveDatabase(path);
  const auto data = MakeDataset(config.rows, config.value_bytes);
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open point benchmark");
  StoreDataset(database, data, false, config);
  Check(database.Checkpoint(), "Checkpoint point fixture");

  auto order = std::vector<std::size_t>(config.rows);
  std::iota(order.begin(), order.end(), 0U);
  auto generator = std::mt19937_64{config.seed ^ (transaction_scoped ? 0x54584EULL : 0x4442554C4C)};
  std::ranges::shuffle(order, generator);
  for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
    (void)ObservePointReads(database, data, order, config, transaction_scoped);
  }

  auto throughput = SampleSet{name, "throughput", "operations/second", {}};
  auto latency = SampleSet{name, "amortized_latency", "nanoseconds/operation", {}};
  auto hits = SampleSet{name, "cache_hit_rate", "ratio", {}};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    auto observation = ObservePointReads(database, data, order, config, transaction_scoped);
    throughput.samples.push_back(observation.operations_per_second);
    latency.samples.push_back(observation.nanoseconds_per_operation);
    hits.samples.push_back(observation.cache_hit_rate);
  }
  Check(database.Close(), "Close point benchmark");
  RemoveDatabase(path);
  return {std::move(throughput), std::move(latency), std::move(hits)};
}

auto ExpectedCursorDigest(const Dataset &data, bool copy_values) -> std::uint64_t {
  auto digest = std::uint64_t{0};
  for (std::size_t row = 0; row < data.keys.size(); ++row) {
    digest += data.keys[row].size() * 257U;
    digest += copy_values ? ValueDigest(data.first_values[row]) : data.first_values[row].size();
  }
  return digest;
}

auto ObserveCursor(tinydb::Database &database, const Dataset &data, const Config &config,
                   bool copy_values) -> PointObservation {
  const auto target = std::chrono::milliseconds(config.minimum_trial_ms);
  auto rows = std::uint64_t{0};
  auto passes = std::uint64_t{0};
  auto digest = std::uint64_t{0};
  const auto started = Clock::now();
  do {
    auto read = Take(database.BeginRead(), "BeginRead cursor");
    auto cursor = Take(read.Scan(), "Scan measured");
    while (cursor.Valid()) {
      digest += cursor.Key().size() * 257U;
      if (copy_values) {
        digest += ValueDigest(Take(cursor.CopyValue(), "Cursor CopyValue measured"));
      } else {
        digest += cursor.ValueSize();
      }
      rows += 1;
      Check(cursor.Next(), "Cursor Next measured");
    }
    passes += 1;
  } while (Clock::now() - started < target);
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  if (rows != data.keys.size() * passes || digest != ExpectedCursorDigest(data, copy_values) * passes) {
    Fail("cursor validation mismatch");
  }
  return PointObservation{
      .operations_per_second = static_cast<double>(rows) / seconds,
      .nanoseconds_per_operation = seconds * 1'000'000'000.0 / static_cast<double>(rows),
      .cache_hit_rate = 0,
  };
}

auto RunCursorBenchmark(const Config &config, bool copy_values) -> std::vector<SampleSet> {
  const auto name = std::string{copy_values ? "cursor_value_scan" : "cursor_metadata_scan"};
  const auto path = Path(name);
  RemoveDatabase(path);
  const auto data = MakeDataset(config.rows, config.value_bytes);
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open cursor benchmark");
  StoreDataset(database, data, false, config);
  Check(database.Checkpoint(), "Checkpoint cursor fixture");
  for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
    (void)ObserveCursor(database, data, config, copy_values);
  }

  auto throughput = SampleSet{name, "throughput", "rows/second", {}};
  auto latency = SampleSet{name, "amortized_latency", "nanoseconds/row", {}};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    const auto observation = ObserveCursor(database, data, config, copy_values);
    throughput.samples.push_back(observation.operations_per_second);
    latency.samples.push_back(observation.nanoseconds_per_operation);
  }
  Check(database.Close(), "Close cursor benchmark");
  RemoveDatabase(path);
  return {std::move(throughput), std::move(latency)};
}

auto ObserveCheckpoint(const Config &config, const Dataset &data) -> CheckpointObservation {
  const auto path = Path("checkpoint");
  RemoveDatabase(path);
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open checkpoint benchmark");
  StoreDataset(database, data, false, config);
  Check(database.Checkpoint(), "Checkpoint initial fixture");
  StoreDataset(database, data, true, config);
  const auto before = Take(database.Stats(), "Stats before checkpoint");
  if (before.dirty_bytes == 0) {
    Fail("checkpoint fixture produced no dirty bytes");
  }

  const auto started = Clock::now();
  Check(database.Checkpoint(), "Checkpoint measured");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after checkpoint");
  if (after.dirty_pages != 0) {
    Fail("measured checkpoint left dirty pages");
  }
  const auto bytes = FileBytes(path);
  Check(database.Close(), "Close checkpoint benchmark");
  RemoveDatabase(path);
  return CheckpointObservation{
      .milliseconds = seconds * 1'000.0,
      .mebibytes_per_second = (static_cast<double>(before.dirty_bytes) / static_cast<double>(1U << 20U)) / seconds,
      .database_bytes = static_cast<double>(bytes),
  };
}

auto RunCheckpointBenchmark(const Config &config) -> std::vector<SampleSet> {
  const auto data = MakeDataset(config.rows, config.value_bytes);
  for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
    (void)ObserveCheckpoint(config, data);
  }
  auto latency = SampleSet{"checkpoint", "latency", "milliseconds", {}};
  auto bandwidth = SampleSet{"checkpoint", "dirty_transfer_rate", "MiB/second", {}};
  auto bytes = SampleSet{"checkpoint", "database_size", "bytes", {}};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    const auto observation = ObserveCheckpoint(config, data);
    latency.samples.push_back(observation.milliseconds);
    bandwidth.samples.push_back(observation.mebibytes_per_second);
    bytes.samples.push_back(observation.database_bytes);
  }
  return {std::move(latency), std::move(bandwidth), std::move(bytes)};
}

[[noreturn]] void RecoveryWriter(const std::filesystem::path &path, const Dataset &data, const Config &config) {
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open recovery writer");
  StoreDataset(database, data, false, config);
  // _exit deliberately skips Database destruction. Every acknowledged fixture
  // transaction is durable, while none of its pages are checkpointed by Close.
  ::_exit(0);
}

void WaitForWriter(pid_t child) {
  auto status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    Fail("recovery writer child failed");
  }
}

/* This is process-restart recovery, not a claim about cold-device latency.
** The writer exits without a TinyDB close, but the harness cannot portably
** evict filesystem cache. The measured Open includes WAL validation, physical
** replay, database synchronization, superblock publication, and WAL cleanup. */
auto ObserveRecovery(const Config &config, const Dataset &data) -> RecoveryObservation {
  const auto path = Path("recovery_process_restart");
  RemoveDatabase(path);
  const auto child = ::fork();
  if (child == 0) {
    RecoveryWriter(path, data, config);
  }
  if (child < 0) {
    Fail("fork failed");
  }
  WaitForWriter(child);
  const auto bytes = PersistentBytes(path);

  const auto started = Clock::now();
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open measured recovery");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto first = Take(database.Get(data.keys.front()), "Get first recovered key");
  const auto last = Take(database.Get(data.keys.back()), "Get last recovered key");
  if (!first || !last || *first != data.first_values.front() || *last != data.first_values.back()) {
    Fail("recovery validation mismatch");
  }
  Check(database.Close(), "Close recovery benchmark");
  RemoveDatabase(path);
  return RecoveryObservation{
      .milliseconds = seconds * 1'000.0,
      .mebibytes_per_second = (static_cast<double>(bytes) / static_cast<double>(1U << 20U)) / seconds,
  };
}

auto RunRecoveryBenchmark(const Config &config) -> std::vector<SampleSet> {
  const auto data = MakeDataset(config.rows, config.value_bytes);
  for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
    (void)ObserveRecovery(config, data);
  }
  auto latency = SampleSet{"recovery_process_restart", "open_latency", "milliseconds", {}};
  auto bandwidth = SampleSet{"recovery_process_restart", "replay_rate", "MiB/second", {}};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    const auto observation = ObserveRecovery(config, data);
    latency.samples.push_back(observation.milliseconds);
    bandwidth.samples.push_back(observation.mebibytes_per_second);
  }
  return {std::move(latency), std::move(bandwidth)};
}

auto ObserveChurn(const Config &config, const Dataset &data) -> ChurnObservation {
  const auto path = Path("churn");
  RemoveDatabase(path);
  auto database = Take(tinydb::Database::Open(path, BenchmarkOptions(config)), "Open churn benchmark");
  const auto started = Clock::now();
  for (std::size_t round = 0; round < config.churn_rounds; ++round) {
    StoreDataset(database, data, round % 2U != 0, config);
    DeleteDataset(database, data);
    Check(database.Checkpoint(), "Checkpoint churn round");
  }
  StoreDataset(database, data, false, config);
  Check(database.Checkpoint(), "Checkpoint final churn state");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto value = Take(database.Get(data.keys.back()), "Get final churn key");
  if (!value || *value != data.first_values.back()) {
    Fail("churn validation mismatch");
  }
  const auto bytes = FileBytes(path);
  Check(database.Close(), "Close churn benchmark");
  RemoveDatabase(path);

  const auto operations_per_round = CheckedMultiply(config.rows, 2U, "churn operations per round");
  const auto operations = CheckedMultiply(operations_per_round, config.churn_rounds, "churn operations") + config.rows;
  return ChurnObservation{
      .operations_per_second = static_cast<double>(operations) / seconds,
      .file_amplification = static_cast<double>(bytes) / static_cast<double>(data.logical_bytes),
      .database_bytes = static_cast<double>(bytes),
  };
}

auto RunChurnBenchmark(const Config &config) -> std::vector<SampleSet> {
  const auto data = MakeDataset(config.rows, config.value_bytes);
  for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
    (void)ObserveChurn(config, data);
  }
  auto throughput = SampleSet{"churn", "throughput", "operations/second", {}};
  auto amplification = SampleSet{"churn", "file_amplification", "bytes/byte", {}};
  auto bytes = SampleSet{"churn", "database_size", "bytes", {}};
  for (std::size_t trial = 0; trial < config.trials; ++trial) {
    const auto observation = ObserveChurn(config, data);
    throughput.samples.push_back(observation.operations_per_second);
    amplification.samples.push_back(observation.file_amplification);
    bytes.samples.push_back(observation.database_bytes);
  }
  return {std::move(throughput), std::move(amplification), std::move(bytes)};
}

void Usage() {
  std::puts(
      "usage: TinyDB_bench [options]\n"
      "  --workload all|writes|reads|scan|checkpoint|recovery|churn\n"
      "  --rows N\n"
      "  --transactions N\n"
      "  --batch N\n"
      "  --value-bytes N\n"
      "  --trials N              (at least 3)\n"
      "  --warmups N             (may be 0)\n"
      "  --minimum-trial-ms N\n"
      "  --churn-rounds N\n"
      "  --cache-bytes N\n"
      "  --seed N");
}

auto ParseUnsigned(std::string_view text, std::string_view flag) -> std::uint64_t {
  auto value = std::uint64_t{0};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    Fail(std::string("invalid ") + std::string(flag));
  }
  return value;
}

auto AsSize(std::uint64_t value, std::string_view flag) -> std::size_t {
  if (value > std::numeric_limits<std::size_t>::max()) {
    Fail(std::string(flag) + " is too large");
  }
  return static_cast<std::size_t>(value);
}

auto Parse(int argc, char **argv) -> Config {
  auto config = Config{};
  for (auto index = 1; index < argc; ++index) {
    const auto flag = std::string_view{argv[index]};
    if (flag == "--help") {
      Usage();
      std::exit(0);
    }
    if (index + 1 >= argc) {
      Fail("missing benchmark option value");
    }
    const auto text = std::string_view{argv[++index]};
    if (flag == "--workload") {
      config.workload = text;
      continue;
    }
    const auto value = ParseUnsigned(text, flag);
    if (flag == "--rows")
      config.rows = AsSize(value, flag);
    else if (flag == "--transactions")
      config.transactions = AsSize(value, flag);
    else if (flag == "--batch")
      config.batch = AsSize(value, flag);
    else if (flag == "--value-bytes")
      config.value_bytes = AsSize(value, flag);
    else if (flag == "--trials")
      config.trials = AsSize(value, flag);
    else if (flag == "--warmups")
      config.warmups = AsSize(value, flag);
    else if (flag == "--minimum-trial-ms")
      config.minimum_trial_ms = AsSize(value, flag);
    else if (flag == "--churn-rounds")
      config.churn_rounds = AsSize(value, flag);
    else if (flag == "--cache-bytes")
      config.cache_bytes = AsSize(value, flag);
    else if (flag == "--seed")
      config.seed = value;
    else
      Fail(std::string("unknown benchmark option: ") + std::string(flag));
  }

  if (config.rows == 0 || config.transactions == 0 || config.batch == 0 || config.value_bytes == 0 ||
      config.minimum_trial_ms == 0 || config.churn_rounds == 0 || config.cache_bytes == 0) {
    Fail("row, transaction, batch, value, duration, churn, and cache settings must be nonzero");
  }
  if (config.trials < 3U) {
    Fail("--trials must be at least 3");
  }
  if (config.value_bytes > tinydb::MAX_VALUE_BYTES) {
    Fail("--value-bytes exceeds TinyDB's value limit");
  }
  const auto measured_payload = CheckedMultiply(config.batch, config.value_bytes, "batch payload");
  if (measured_payload > (8U << 20U)) {
    Fail("batch payload must not exceed 8 MiB");
  }
  (void)CheckedMultiply(config.transactions, config.batch, "transactions times batch");
  constexpr auto workloads =
      std::array<std::string_view, 7>{"all", "writes", "reads", "scan", "checkpoint", "recovery", "churn"};
  if (std::ranges::find(workloads, config.workload) == workloads.end()) {
    Fail("unknown --workload value");
  }
  return config;
}

auto Selected(const Config &config, std::string_view workload) -> bool {
  return config.workload == "all" || config.workload == workload;
}

void PrintConfiguration(const Config &config) {
  std::fprintf(stderr,
               "TinyDB benchmark: workload=%s rows=%zu transactions=%zu batch=%zu value_bytes=%zu trials=%zu "
               "warmups=%zu minimum_trial_ms=%zu churn_rounds=%zu cache_bytes=%zu seed=%llu\n",
               config.workload.c_str(), config.rows, config.transactions, config.batch, config.value_bytes,
               config.trials, config.warmups, config.minimum_trial_ms, config.churn_rounds, config.cache_bytes,
               static_cast<unsigned long long>(config.seed));
}

}  // namespace

auto main(int argc, char **argv) -> int {
  const auto config = Parse(argc, argv);
  PrintConfiguration(config);
  std::puts("benchmark,metric,unit,samples,mean,stddev,min,p50,p95,p99,max");
  if (Selected(config, "writes")) {
    Print(RunWriteBenchmark(config, false));
    Print(RunWriteBenchmark(config, true));
  }
  if (Selected(config, "reads")) {
    Print(RunPointBenchmark(config, false));
    Print(RunPointBenchmark(config, true));
  }
  if (Selected(config, "scan")) {
    Print(RunCursorBenchmark(config, false));
    Print(RunCursorBenchmark(config, true));
  }
  if (Selected(config, "checkpoint")) {
    Print(RunCheckpointBenchmark(config));
  }
  if (Selected(config, "recovery")) {
    Print(RunRecoveryBenchmark(config));
  }
  if (Selected(config, "churn")) {
    Print(RunChurnBenchmark(config));
  }
  return 0;
}
