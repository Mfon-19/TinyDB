#include "benchmark.h"

#include "api/database_test_access.h"

#include <tinydb/database.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tinydb::bench {

namespace {

auto CheckedMultiply(std::size_t left, std::size_t right, std::string_view description) -> std::size_t {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    Fail(std::string(description) + " is too large");
  }
  return left * right;
}

template <typename T>
auto Take(Result<T> result, std::string_view operation) -> T {
  if (!result) {
    Fail(std::string(operation) + ": " + result.error().ToString());
  }
  return std::move(*result);
}

void Check(const Status &status, std::string_view operation) {
  if (!status.Ok()) {
    Fail(std::string(operation) + ": " + status.ToString());
  }
}

auto DatabasePath(const std::filesystem::path &root) -> std::filesystem::path { return root / "database.db"; }

auto DatabaseFileBytes(const std::filesystem::path &root) -> std::uint64_t {
  auto ignored = std::error_code{};
  const auto path = DatabasePath(root);
  return std::filesystem::exists(path, ignored) ? std::filesystem::file_size(path, ignored) : 0;
}

auto PersistentBytes(const std::filesystem::path &root) -> std::uint64_t {
  const auto path = DatabasePath(root);
  auto bytes = DatabaseFileBytes(root);
  auto ignored = std::error_code{};
  const auto wal_prefix = path.filename().string() + "-wal";
  if (!std::filesystem::exists(root, ignored)) {
    return bytes;
  }
  for (const auto &entry : std::filesystem::directory_iterator(root, ignored)) {
    if (entry.path().filename().string().starts_with(wal_prefix) && entry.is_regular_file(ignored)) {
      bytes += entry.file_size(ignored);
    }
  }
  return bytes;
}

auto BenchmarkOptions(const Scenario &scenario, std::optional<std::size_t> page_cache_bytes = std::nullopt,
                      IoMode io_mode = IoMode::Buffered) -> Options {
  auto options = Options{};
  options.page_io_mode = io_mode == IoMode::Direct ? PageIoMode::Direct : PageIoMode::Buffered;
  options.page_cache_bytes = page_cache_bytes.value_or(scenario.page_cache_bytes);
  const auto transaction_payload =
      CheckedMultiply(scenario.batch, scenario.key_bytes + scenario.value_bytes, "transaction payload");
  options.max_write_transaction_bytes = std::max<std::size_t>(32U << 20U, transaction_payload * 3U);
  options.checkpoint.wal_trigger_bytes = std::numeric_limits<std::uint64_t>::max();
  options.checkpoint.dirty_trigger_bytes = std::numeric_limits<std::size_t>::max();
  options.checkpoint.hard_wal_bytes = std::numeric_limits<std::uint64_t>::max();
  options.checkpoint.hard_dirty_bytes = std::numeric_limits<std::size_t>::max();
  options.checkpoint.maximum_age = std::chrono::hours(24);
  return options;
}

void StoreDataset(Database &database, const Dataset &data, bool second, const Scenario &scenario) {
  const auto &values = second ? data.second_values : data.first_values;
  const auto row_bytes = std::max<std::size_t>(1, scenario.key_bytes + scenario.value_bytes);
  const auto batch = std::max<std::size_t>(1, std::min<std::size_t>(256, (4U << 20U) / row_bytes));
  for (std::size_t first = 0; first < data.keys.size(); first += batch) {
    auto write = Take(database.BeginWrite(), "BeginWrite fixture");
    for (std::size_t row = first; row < std::min(first + batch, data.keys.size()); ++row) {
      Check(write.Put(data.keys[row], values[row]), "Put fixture");
    }
    (void)Take(std::move(write).Commit(), "Commit fixture");
  }
}

auto ValueDigest(std::string_view value) -> std::uint64_t {
  if (value.empty()) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.size()) * 131U + static_cast<unsigned char>(value.front()) * 17U +
         static_cast<unsigned char>(value.back());
}

using Clock = std::chrono::steady_clock;

struct Resources final {
  ProcessUsage usage;
  ProcessIo io;
  ProcessMemory memory_before_open;
  ProcessMemory memory_endpoint;
};

struct ResourceStart final {
  ProcessUsage usage;
  ProcessIo io;
};

struct LifecycleObservation final {
  double seconds{0};
  double milliseconds{0};
  double mebibytes_per_second{0};
  double bytes{0};
  double dirty_bytes{0};
  std::uint64_t cache_resident_bytes{0};
  FileResidency residency;
  Resources resources;
};

struct IoReadObservation final {
  double open_milliseconds{0};
  double workload_seconds{0};
  double workload_milliseconds{0};
  double operations_per_second{0};
  double cache_hit_rate{0};
  std::uint64_t cache_resident_bytes{0};
  FileResidency before_open_residency;
  FileResidency before_workload_residency;
  FileResidency pre_close_residency;
  FileResidency post_close_residency;
  ProcessIo open_io;
  ProcessIo workload_io;
  ProcessIo close_io;
  ProcessIo process_io;
  ProcessUsage workload_usage;
  ProcessMemory memory_before_open;
  ProcessMemory memory_endpoint;
  double logical_read_bytes{0};
  ReadAheadCounters read_ahead;
};

auto StartResources() -> ResourceStart { return ResourceStart{ObserveProcessUsage(), ObserveProcessIo()}; }

auto FinishResources(const ResourceStart &start, ProcessMemory before_open) -> Resources {
  return Resources{
      .usage = SubtractProcessUsage(ObserveProcessUsage(), start.usage),
      .io = SubtractProcessIo(ObserveProcessIo(), start.io),
      .memory_before_open = before_open,
      .memory_endpoint = ObserveProcessMemory(),
  };
}

auto CounterDelta(std::uint64_t after, std::uint64_t before, std::string_view name) -> std::uint64_t {
  if (after < before) {
    Fail(std::string(name) + " counter moved backward");
  }
  return after - before;
}

auto SubtractReadAhead(const ReadAheadCounters &after, const ReadAheadCounters &before) -> ReadAheadCounters {
  return {
      .plans = CounterDelta(after.plans, before.plans, "read-ahead plans"),
      .pages_scheduled = CounterDelta(after.pages_scheduled, before.pages_scheduled, "scheduled read-ahead pages"),
      .pages_consumed = CounterDelta(after.pages_consumed, before.pages_consumed, "consumed read-ahead pages"),
  };
}

auto NearestRank(std::vector<double> values, double percentile) -> double {
  if (values.empty()) {
    Fail("cannot summarize an empty observation population");
  }
  std::ranges::sort(values);
  const auto rank = static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(values.size())));
  return values[std::max<std::size_t>(1, rank) - 1U];
}

void AddResources(const Scenario &scenario, Results &results, std::size_t trial, const Resources &resources,
                  double elapsed_seconds) {
  const auto cpu_seconds = resources.usage.user_seconds + resources.usage.system_seconds;
  results.AddTrial(scenario, "user_cpu_time", "seconds", trial, resources.usage.user_seconds);
  results.AddTrial(scenario, "system_cpu_time", "seconds", trial, resources.usage.system_seconds);
  results.AddTrial(scenario, "cpu_utilization", "cores", trial,
                   elapsed_seconds == 0.0 ? 0.0 : cpu_seconds / elapsed_seconds);
  results.AddTrial(scenario, "minor_faults", "faults", trial, static_cast<double>(resources.usage.minor_faults));
  results.AddTrial(scenario, "major_faults", "faults", trial, static_cast<double>(resources.usage.major_faults));
  results.AddTrial(scenario, "voluntary_context_switches", "switches", trial,
                   static_cast<double>(resources.usage.voluntary_context_switches));
  results.AddTrial(scenario, "involuntary_context_switches", "switches", trial,
                   static_cast<double>(resources.usage.involuntary_context_switches));
  results.AddTrial(scenario, "storage_read_bytes", "bytes", trial,
                   static_cast<double>(resources.io.storage_read_bytes));
  results.AddTrial(scenario, "storage_write_bytes", "bytes", trial,
                   static_cast<double>(resources.io.storage_write_bytes));
  results.AddTrial(scenario, "read_syscalls", "calls", trial, static_cast<double>(resources.io.read_syscalls));
  results.AddTrial(scenario, "write_syscalls", "calls", trial, static_cast<double>(resources.io.write_syscalls));
  results.AddTrial(scenario, "process_rss_before_open", "bytes", trial,
                   static_cast<double>(resources.memory_before_open.resident_bytes));
  results.AddTrial(scenario, "process_rss_endpoint", "bytes", trial,
                   static_cast<double>(resources.memory_endpoint.resident_bytes));
  results.AddTrial(scenario, "process_rss_growth", "bytes", trial,
                   static_cast<double>(resources.memory_endpoint.resident_bytes) -
                       static_cast<double>(resources.memory_before_open.resident_bytes));
  results.AddTrial(scenario, "process_pss_before_open", "bytes", trial,
                   static_cast<double>(resources.memory_before_open.proportional_bytes));
  results.AddTrial(scenario, "process_pss_endpoint", "bytes", trial,
                   static_cast<double>(resources.memory_endpoint.proportional_bytes));
}

void AddCache(const Scenario &scenario, Results &results, std::size_t trial, std::uint64_t cache_resident_bytes,
              const FileResidency &residency) {
  results.AddTrial(scenario, "tinydb_cache_resident_bytes", "bytes", trial, static_cast<double>(cache_resident_bytes));
  results.AddTrial(scenario, "database_file_resident_bytes", "bytes", trial,
                   static_cast<double>(residency.resident_bytes));
  results.AddTrial(scenario, "database_file_resident_ratio", "ratio", trial, residency.Ratio());
  results.AddTrial(scenario, "combined_cache_resident_bytes", "bytes", trial,
                   static_cast<double>(cache_resident_bytes + residency.resident_bytes));
}

void AddReadAhead(const Scenario &scenario, Results &results, std::size_t trial, const ReadAheadCounters &read_ahead) {
  results.AddTrial(scenario, "readahead_plans", "plans", trial, static_cast<double>(read_ahead.plans));
  results.AddTrial(scenario, "readahead_pages_scheduled", "pages", trial,
                   static_cast<double>(read_ahead.pages_scheduled));
  results.AddTrial(scenario, "readahead_pages_consumed", "pages", trial,
                   static_cast<double>(read_ahead.pages_consumed));
}

void AddCommitLatencies(const Scenario &scenario, Results &results, std::size_t trial,
                        const std::vector<double> &latencies) {
  results.AddTrial(scenario, "commit_latency_p50", "microseconds", trial, NearestRank(latencies, 0.50));
  results.AddTrial(scenario, "commit_latency_p95", "microseconds", trial, NearestRank(latencies, 0.95));
  for (std::size_t index = 0; index < latencies.size(); ++index) {
    results.AddObservation(scenario, "commit_latency", "microseconds", trial, index, latencies[index]);
  }
}

auto DropAndRequireCold(const std::filesystem::path &path, std::string_view phase) -> FileResidency {
  if (!AdviseDropFileCache(path)) {
    Fail(std::string("Linux rejected the ") + std::string(phase) + " file-cache eviction request");
  }
  const auto residency = ObserveFileResidency(path);
  const auto allowed = std::max<std::uint64_t>(64U << 10U, residency.file_bytes / 1'000U);
  if (residency.resident_bytes > allowed) {
    Fail(std::string(phase) + " file-cache residency exceeds the cold-trial limit");
  }
  return residency;
}

void PreparePreOpenCacheState(const Scenario &scenario, const std::filesystem::path &path) {
  if (scenario.cache_condition == CacheCondition::OsWarm) {
    WarmDatabaseFamily(path);
  } else {
    (void)DropAndRequireCold(path, "pre-open");
  }
}

void PrimeEngineReadState(Database &database, const Scenario &scenario, const Dataset &data) {
  if (scenario.cache_condition != CacheCondition::EngineHot && scenario.cache_condition != CacheCondition::Steady) {
    return;
  }
  auto read = Take(database.BeginRead(), "BeginRead cache priming");
  auto cursor = Take(read.Scan(), "Scan cache priming");
  auto row = std::size_t{0};
  while (cursor.Valid()) {
    if (row >= data.keys.size() || cursor.Key() != data.keys[row] ||
        Take(cursor.CopyValue(), "Copy cache-priming value") != data.first_values[row]) {
      Fail("cache-priming scan returned unexpected data");
    }
    ++row;
    Check(cursor.Next(), "Advance cache-priming scan");
  }
  if (row != data.keys.size()) {
    Fail("cache-priming scan did not visit the complete fixture");
  }
}

struct ConcurrentObservation final {
  double seconds{0};
  double reader_operations_per_second{0};
  double writer_operations_per_second{0};
  std::uint64_t cache_resident_bytes{0};
  Resources resources;
  std::vector<double> commit_microseconds;
};

auto ObserveConcurrent(Database &database, const Scenario &scenario, const Dataset &data, std::uint64_t seed,
                       ProcessMemory memory_before_open) -> ConcurrentObservation {
  auto start = std::atomic<bool>{false};
  auto stop = std::atomic<bool>{false};
  auto reader_counts = std::vector<std::uint64_t>(scenario.reader_threads, 0);
  auto readers = std::vector<std::thread>{};
  readers.reserve(scenario.reader_threads);
  for (std::size_t reader = 0; reader < scenario.reader_threads; ++reader) {
    readers.emplace_back([&, reader] {
      auto generator = std::mt19937_64{seed ^ reader};
      auto count = std::uint64_t{0};
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      while (!stop.load(std::memory_order_acquire)) {
        auto read = Take(database.BeginRead(), "BeginRead concurrent");
        for (std::size_t item = 0; item < 16U; ++item) {
          const auto row = static_cast<std::size_t>(generator() % scenario.rows);
          const auto value = Take(read.Get(data.keys[row]), "Get concurrent");
          if (!value || (*value != data.first_values[row] && *value != data.second_values[row])) {
            Fail("concurrent reader observed an invalid value");
          }
          count += 1;
        }
      }
      reader_counts[reader] = count;
    });
  }

  auto latencies = std::vector<double>{};
  latencies.reserve(scenario.commits);
  auto generator = std::mt19937_64{seed ^ 0x575249544552ULL};
  const auto resources_started = StartResources();
  start.store(true, std::memory_order_release);
  const auto started = Clock::now();
  for (std::size_t transaction = 0; transaction < scenario.commits; ++transaction) {
    auto write = Take(database.BeginWrite(), "BeginWrite concurrent");
    for (std::size_t item = 0; item < scenario.batch; ++item) {
      const auto row = static_cast<std::size_t>(generator() % scenario.rows);
      Check(write.Put(data.keys[row], data.second_values[row]), "Put concurrent");
    }
    const auto commit_started = Clock::now();
    (void)Take(std::move(write).Commit(), "Commit concurrent");
    latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - commit_started).count());
  }
  stop.store(true, std::memory_order_release);
  for (auto &reader : readers) {
    reader.join();
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto stats = Take(database.Stats(), "Stats after concurrent workload");
  const auto resources = FinishResources(resources_started, memory_before_open);
  return ConcurrentObservation{
      .seconds = seconds,
      .reader_operations_per_second =
          static_cast<double>(std::accumulate(reader_counts.begin(), reader_counts.end(), std::uint64_t{0})) / seconds,
      .writer_operations_per_second = static_cast<double>(scenario.commits * scenario.batch) / seconds,
      .cache_resident_bytes = stats.cache_resident_bytes,
      .resources = resources,
      .commit_microseconds = std::move(latencies),
  };
}

auto PrepareCheckpointWrites(Database &database, const Scenario &scenario, const Dataset &data) -> std::size_t {
  const auto row_bytes = scenario.key_bytes + scenario.value_bytes + 64U;
  const auto rows = std::min(data.keys.size(), std::max<std::size_t>(1, scenario.page_cache_bytes / (4U * row_bytes)));
  constexpr auto batch = std::size_t{256};
  for (std::size_t first = 0; first < rows; first += batch) {
    auto write = Take(database.BeginWrite(), "BeginWrite checkpoint setup");
    for (std::size_t row = first; row < std::min(first + batch, rows); ++row) {
      Check(write.Put(data.keys[row], data.second_values[row]), "Put checkpoint setup");
    }
    (void)Take(std::move(write).Commit(), "Commit checkpoint setup");
  }
  return rows;
}

auto ObserveCheckpoint(const std::filesystem::path &path, const Scenario &scenario, const Dataset &data,
                       const Options &options) -> LifecycleObservation {
  PreparePreOpenCacheState(scenario, path);
  const auto memory_before_open = ObserveProcessMemory();
  auto database = Take(Database::Open(DatabasePath(path), options), "Open checkpoint benchmark");
  const auto updated_rows = PrepareCheckpointWrites(database, scenario, data);
  const auto before = Take(database.Stats(), "Stats before checkpoint");
  if (before.dirty_bytes == 0) {
    Fail("checkpoint fixture produced no dirty bytes");
  }
  const auto resources_started = StartResources();
  const auto started = Clock::now();
  Check(database.Checkpoint(), "Checkpoint measured");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after checkpoint");
  auto resources = FinishResources(resources_started, memory_before_open);
  if (after.dirty_pages != 0) {
    Fail("checkpoint left dirty pages");
  }
  const auto first = Take(database.Get(data.keys.front()), "Get first checkpointed key");
  const auto last = Take(database.Get(data.keys[updated_rows - 1U]), "Get last checkpointed key");
  if (!first || !last || *first != data.second_values.front() || *last != data.second_values[updated_rows - 1U]) {
    Fail("checkpoint validation failed");
  }
  const auto bytes = DatabaseFileBytes(path);
  Check(database.Close(), "Close checkpoint benchmark");
  return LifecycleObservation{
      .seconds = seconds,
      .milliseconds = seconds * 1'000.0,
      .mebibytes_per_second = (static_cast<double>(before.dirty_bytes) / static_cast<double>(1U << 20U)) / seconds,
      .bytes = static_cast<double>(bytes),
      .dirty_bytes = static_cast<double>(before.dirty_bytes),
      .cache_resident_bytes = after.cache_resident_bytes,
      .residency = ObserveFileResidency(path),
      .resources = resources,
  };
}

[[noreturn]] void RecoveryWriter(const std::filesystem::path &path, const Dataset &data, const Scenario &scenario,
                                 IoMode io_mode) {
  auto database = Take(Database::Open(DatabasePath(path), BenchmarkOptions(scenario, std::nullopt, io_mode)),
                       "Open recovery writer");
  StoreDataset(database, data, false, scenario);
  ::_exit(0);
}

void WaitForWriter(pid_t child) {
  auto status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    Fail("recovery writer child failed");
  }
}

auto ObserveRecovery(const std::filesystem::path &path, const Scenario &scenario, const Dataset &data,
                     const Options &options) -> LifecycleObservation {
  PreparePreOpenCacheState(scenario, path);
  const auto bytes = PersistentBytes(path);
  const auto memory_before_open = ObserveProcessMemory();
  const auto resources_started = StartResources();
  const auto started = Clock::now();
  auto database = Take(Database::Open(DatabasePath(path), options), "Open measured recovery");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto stats = Take(database.Stats(), "Stats after recovery");
  auto resources = FinishResources(resources_started, memory_before_open);
  const auto first = Take(database.Get(data.keys.front()), "Get first recovered key");
  const auto last = Take(database.Get(data.keys.back()), "Get last recovered key");
  if (!first || !last || *first != data.first_values.front() || *last != data.first_values.back()) {
    Fail("recovery validation failed");
  }
  Check(database.Close(), "Close recovery benchmark");
  return LifecycleObservation{
      .seconds = seconds,
      .milliseconds = seconds * 1'000.0,
      .mebibytes_per_second = (static_cast<double>(bytes) / static_cast<double>(1U << 20U)) / seconds,
      .bytes = static_cast<double>(bytes),
      .cache_resident_bytes = stats.cache_resident_bytes,
      .residency = ObserveFileResidency(path),
      .resources = resources,
  };
}

auto ObserveIoRead(const std::filesystem::path &path, const Scenario &scenario, const Dataset &data,
                   const Options &options, const std::vector<std::size_t> &random_plan,
                   std::uint64_t expected_scan_digest) -> IoReadObservation {
  const auto before_open_residency = DropAndRequireCold(path, "pre-open");
  const auto memory_before_open = ObserveProcessMemory();
  const auto process_io_before = ObserveProcessIo();
  const auto open_io_before = ObserveProcessIo();
  const auto open_started = Clock::now();
  auto database = Take(Database::Open(DatabasePath(path), options), "Open cold-I/O benchmark");
  const auto open_seconds = std::chrono::duration<double>(Clock::now() - open_started).count();
  DatabaseTestAccess::WaitForReadQuiescence(database);
  const auto open_io = SubtractProcessIo(ObserveProcessIo(), open_io_before);
  const auto stats_before = Take(database.Stats(), "Stats before cold reads");
  const auto read_ahead_before = DatabaseTestAccess::ReadAhead(database);

  const auto before_workload_residency = DropAndRequireCold(path, "pre-workload");
  const auto workload_io_before = ObserveProcessIo();
  const auto workload_usage_before = ObserveProcessUsage();
  auto operations = std::uint64_t{0};
  auto digest = std::uint64_t{0};
  const auto workload_started = Clock::now();
  if (scenario.access == AccessPattern::Sequential) {
    auto read = Take(database.BeginRead(), "BeginRead cold scan");
    auto cursor = Take(read.Scan(), "Scan cold fixture");
    while (cursor.Valid()) {
      digest += cursor.Key().size() * 257U;
      digest += ValueDigest(Take(cursor.CopyValue(), "Copy cold scan value"));
      operations += 1;
      Check(cursor.Next(), "Advance cold scan");
    }
    if (operations != scenario.rows || digest != expected_scan_digest) {
      Fail("cold scan validation failed");
    }
  } else {
    auto read = Take(database.BeginRead(), "BeginRead cold random reads");
    for (const auto row : random_plan) {
      const auto value = Take(read.Get(data.keys[row]), "Get cold random key");
      if (!value || *value != data.first_values[row]) {
        Fail("cold random-read validation failed");
      }
      digest += ValueDigest(*value);
      operations += 1;
    }
    if (operations != random_plan.size() || digest == 0) {
      Fail("cold random-read plan was not fully executed");
    }
  }
  DatabaseTestAccess::WaitForReadQuiescence(database);
  const auto workload_seconds = std::chrono::duration<double>(Clock::now() - workload_started).count();
  const auto workload_usage = SubtractProcessUsage(ObserveProcessUsage(), workload_usage_before);
  const auto workload_io = SubtractProcessIo(ObserveProcessIo(), workload_io_before);
  const auto stats_after = Take(database.Stats(), "Stats after cold reads");
  const auto read_ahead_after = DatabaseTestAccess::ReadAhead(database);
  const auto memory_endpoint = ObserveProcessMemory();
  const auto pre_close_residency = ObserveFileResidency(path);
  const auto close_io_before = ObserveProcessIo();
  Check(database.Close(), "Close cold-I/O benchmark");
  const auto close_io = SubtractProcessIo(ObserveProcessIo(), close_io_before);
  const auto process_io = SubtractProcessIo(ObserveProcessIo(), process_io_before);
  return IoReadObservation{
      .open_milliseconds = open_seconds * 1'000.0,
      .workload_seconds = workload_seconds,
      .workload_milliseconds = workload_seconds * 1'000.0,
      .operations_per_second = static_cast<double>(operations) / workload_seconds,
      .cache_hit_rate =
          [&] {
            const auto hits = stats_after.cache_hits - stats_before.cache_hits;
            const auto misses = stats_after.cache_misses - stats_before.cache_misses;
            return hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses);
          }(),
      .cache_resident_bytes = stats_after.cache_resident_bytes,
      .before_open_residency = before_open_residency,
      .before_workload_residency = before_workload_residency,
      .pre_close_residency = pre_close_residency,
      .post_close_residency = ObserveFileResidency(path),
      .open_io = open_io,
      .workload_io = workload_io,
      .close_io = close_io,
      .process_io = process_io,
      .workload_usage = workload_usage,
      .memory_before_open = memory_before_open,
      .memory_endpoint = memory_endpoint,
      .logical_read_bytes =
          static_cast<double>(operations) * static_cast<double>(scenario.key_bytes + scenario.value_bytes),
      .read_ahead = SubtractReadAhead(read_ahead_after, read_ahead_before),
  };
}

void ChurnDeletePhase(Database &database, const Scenario &scenario, const Dataset &data) {
  constexpr auto transaction_rows = std::size_t{256};
  const auto row_bytes = scenario.key_bytes + scenario.value_bytes + 64U;
  const auto checkpoint_rows = std::max<std::size_t>(transaction_rows, scenario.page_cache_bytes / (4U * row_bytes));
  for (std::size_t checkpoint_first = 0; checkpoint_first < data.keys.size(); checkpoint_first += checkpoint_rows) {
    const auto checkpoint_end = std::min(checkpoint_first + checkpoint_rows, data.keys.size());
    for (std::size_t first = checkpoint_first; first < checkpoint_end; first += transaction_rows) {
      auto write = Take(database.BeginWrite(), "BeginWrite churn delete");
      for (std::size_t row = first; row < std::min(first + transaction_rows, checkpoint_end); ++row) {
        Check(write.Delete(data.keys[row]), "Delete churn key");
      }
      (void)Take(std::move(write).Commit(), "Commit churn delete");
    }
    Check(database.Checkpoint(), "Checkpoint churn delete phase");
  }
}

void ChurnStorePhase(Database &database, const Scenario &scenario, const Dataset &data, bool second) {
  constexpr auto transaction_rows = std::size_t{256};
  const auto row_bytes = scenario.key_bytes + scenario.value_bytes + 64U;
  const auto checkpoint_rows = std::max<std::size_t>(transaction_rows, scenario.page_cache_bytes / (4U * row_bytes));
  const auto &values = second ? data.second_values : data.first_values;
  for (std::size_t checkpoint_first = 0; checkpoint_first < data.keys.size(); checkpoint_first += checkpoint_rows) {
    const auto checkpoint_end = std::min(checkpoint_first + checkpoint_rows, data.keys.size());
    for (std::size_t first = checkpoint_first; first < checkpoint_end; first += transaction_rows) {
      auto write = Take(database.BeginWrite(), "BeginWrite churn reinsert");
      for (std::size_t row = first; row < std::min(first + transaction_rows, checkpoint_end); ++row) {
        Check(write.Put(data.keys[row], values[row]), "Reinsert churn key");
      }
      (void)Take(std::move(write).Commit(), "Commit churn reinsert");
    }
    Check(database.Checkpoint(), "Checkpoint churn reinsert phase");
  }
}

auto ChurnRound(Database &database, const Scenario &scenario, const Dataset &data, bool second) -> double {
  const auto started = Clock::now();
  ChurnDeletePhase(database, scenario, data);
  ChurnStorePhase(database, scenario, data, second);
  return std::chrono::duration<double>(Clock::now() - started).count();
}

void RunChurnTrial(const std::filesystem::path &path, const Scenario &scenario, const Dataset &data, Results &results,
                   const Options &options, std::size_t trial) {
  PreparePreOpenCacheState(scenario, path);
  const auto memory_before_open = ObserveProcessMemory();
  auto database = Take(Database::Open(DatabasePath(path), options), "Open churn benchmark");
  for (std::size_t round = 0; round < scenario.churn_warmup_rounds; ++round) {
    (void)ChurnRound(database, scenario, data, round % 2U == 0);
  }

  auto throughputs = std::vector<double>{};
  auto amplifications = std::vector<double>{};
  auto first_bytes = std::uint64_t{0};
  auto last_bytes = std::uint64_t{0};
  const auto resources_started = StartResources();
  const auto measured_started = Clock::now();
  for (std::size_t round = 0; round < scenario.churn_measured_rounds; ++round) {
    const auto seconds = ChurnRound(database, scenario, data, round % 2U != 0);
    const auto bytes = DatabaseFileBytes(path);
    if (round == 0) {
      first_bytes = bytes;
    }
    last_bytes = bytes;
    const auto throughput = static_cast<double>(scenario.rows * 2U) / seconds;
    const auto amplification = static_cast<double>(bytes) / static_cast<double>(data.logical_bytes);
    throughputs.push_back(throughput);
    amplifications.push_back(amplification);
    results.AddObservation(scenario, "round_throughput", "operations/second", trial, round, throughput);
    results.AddObservation(scenario, "round_file_amplification", "bytes/byte", trial, round, amplification);
  }
  const auto measured_seconds = std::chrono::duration<double>(Clock::now() - measured_started).count();
  const auto stats = Take(database.Stats(), "Stats after churn");
  const auto resources = FinishResources(resources_started, memory_before_open);
  const auto value = Take(database.Get(data.keys.back()), "Get final churn key");
  const auto final_uses_second = (scenario.churn_measured_rounds - 1U) % 2U != 0;
  const auto &expected = final_uses_second ? data.second_values.back() : data.first_values.back();
  if (!value || *value != expected) {
    Fail("churn validation failed");
  }
  const auto residency = ObserveFileResidency(path);
  Check(database.Close(), "Close churn benchmark");

  results.AddTrial(scenario, "throughput", "operations/second", trial, NearestRank(throughputs, 0.50));
  results.AddTrial(scenario, "file_amplification", "bytes/byte", trial, NearestRank(amplifications, 0.50));
  results.AddTrial(scenario, "database_size", "bytes", trial, static_cast<double>(last_bytes));
  results.AddTrial(scenario, "growth", "bytes/round", trial,
                   (static_cast<double>(last_bytes) - static_cast<double>(first_bytes)) /
                       static_cast<double>(std::max<std::size_t>(1, scenario.churn_measured_rounds - 1U)));
  AddResources(scenario, results, trial, resources, measured_seconds);
  AddCache(scenario, results, trial, stats.cache_resident_bytes, residency);
}

void AddLifecycleResults(const Scenario &scenario, Results &results, std::size_t trial,
                         const LifecycleObservation &observation, bool recovery) {
  results.AddTrial(scenario, recovery ? "open_latency" : "latency", "milliseconds", trial, observation.milliseconds);
  results.AddTrial(scenario, recovery ? "replay_rate" : "dirty_transfer_rate", "MiB/second", trial,
                   observation.mebibytes_per_second);
  results.AddTrial(scenario, recovery ? "persistent_size" : "database_size", "bytes", trial, observation.bytes);
  if (!recovery) {
    results.AddTrial(scenario, "storage_write_amplification", "bytes/dirty_byte", trial,
                     observation.dirty_bytes == 0.0
                         ? 0.0
                         : static_cast<double>(observation.resources.io.storage_write_bytes) / observation.dirty_bytes);
  }
  AddResources(scenario, results, trial, observation.resources, observation.seconds);
  AddCache(scenario, results, trial, observation.cache_resident_bytes, observation.residency);
}

void AddIoReadResults(const Scenario &scenario, Results &results, std::size_t trial,
                      const IoReadObservation &observation) {
  results.AddTrial(scenario, "open_latency", "milliseconds", trial, observation.open_milliseconds);
  results.AddTrial(scenario, "workload_latency", "milliseconds", trial, observation.workload_milliseconds);
  results.AddTrial(scenario, "throughput",
                   scenario.access == AccessPattern::Sequential ? "rows/second" : "reads/second", trial,
                   observation.operations_per_second);
  results.AddTrial(scenario, "cache_hit_rate", "ratio", trial, observation.cache_hit_rate);
  results.AddTrial(scenario, "database_file_pre_open_resident_bytes", "bytes", trial,
                   static_cast<double>(observation.before_open_residency.resident_bytes));
  results.AddTrial(scenario, "database_file_pre_workload_resident_bytes", "bytes", trial,
                   static_cast<double>(observation.before_workload_residency.resident_bytes));
  results.AddTrial(scenario, "database_file_post_close_resident_bytes", "bytes", trial,
                   static_cast<double>(observation.post_close_residency.resident_bytes));
  results.AddTrial(scenario, "total_storage_read_bytes", "bytes", trial,
                   static_cast<double>(observation.process_io.storage_read_bytes));
  results.AddTrial(scenario, "total_storage_write_bytes", "bytes", trial,
                   static_cast<double>(observation.process_io.storage_write_bytes));
  results.AddTrial(scenario, "open_storage_read_bytes", "bytes", trial,
                   static_cast<double>(observation.open_io.storage_read_bytes));
  results.AddTrial(scenario, "open_storage_write_bytes", "bytes", trial,
                   static_cast<double>(observation.open_io.storage_write_bytes));
  results.AddTrial(scenario, "workload_storage_read_bytes", "bytes", trial,
                   static_cast<double>(observation.workload_io.storage_read_bytes));
  results.AddTrial(scenario, "workload_storage_write_bytes", "bytes", trial,
                   static_cast<double>(observation.workload_io.storage_write_bytes));
  results.AddTrial(scenario, "workload_read_syscalls", "calls", trial,
                   static_cast<double>(observation.workload_io.read_syscalls));
  results.AddTrial(scenario, "close_storage_read_bytes", "bytes", trial,
                   static_cast<double>(observation.close_io.storage_read_bytes));
  results.AddTrial(scenario, "close_storage_write_bytes", "bytes", trial,
                   static_cast<double>(observation.close_io.storage_write_bytes));
  results.AddTrial(
      scenario, "workload_storage_read_amplification", "bytes/logical_byte", trial,
      observation.logical_read_bytes == 0.0
          ? 0.0
          : static_cast<double>(observation.workload_io.storage_read_bytes) / observation.logical_read_bytes);
  const auto resources = Resources{
      .usage = observation.workload_usage,
      .io = observation.workload_io,
      .memory_before_open = observation.memory_before_open,
      .memory_endpoint = observation.memory_endpoint,
  };
  AddResources(scenario, results, trial, resources, observation.workload_seconds);
  AddCache(scenario, results, trial, observation.cache_resident_bytes, observation.pre_close_residency);
  AddReadAhead(scenario, results, trial, observation.read_ahead);
}

}  // namespace

void BuildTinyDbFixture(const std::filesystem::path &path, const Scenario &scenario, const Config &config) {
  auto error = std::error_code{};
  if (std::filesystem::exists(path, error) || PersistentBytes(path) != 0) {
    Fail("fixture database family already exists");
  }
  if (error) {
    Fail("cannot inspect fixture database path: " + error.message());
  }

  std::filesystem::create_directories(path, error);
  if (error) {
    Fail("cannot create fixture root: " + error.message());
  }
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  if (scenario.workload == Workload::Recovery) {
    const auto child = ::fork();
    if (child == 0) {
      RecoveryWriter(path, data, scenario, config.io_mode);
    }
    if (child < 0) {
      Fail("fork failed");
    }
    WaitForWriter(child);
    if (!std::filesystem::exists(DatabasePath(path), error) || PersistentBytes(path) <= DatabaseFileBytes(path)) {
      Fail("recovery fixture did not preserve committed WAL");
    }
    return;
  }

  auto database = Take(Database::Open(DatabasePath(path), BenchmarkOptions(scenario, std::nullopt, config.io_mode)),
                       "Open fixture database");
  StoreDataset(database, data, false, scenario);
  Check(database.Checkpoint(), "Checkpoint fixture");
  Check(database.Close(), "Close fixture database");
}

void RunTinyDbTrial(const std::filesystem::path &path, const Scenario &scenario, const Config &config,
                    Results &results) {
  auto error = std::error_code{};
  if (!std::filesystem::is_directory(path, error) || !std::filesystem::is_regular_file(DatabasePath(path), error)) {
    Fail("trial requires an existing database fixture root");
  }
  if (error) {
    Fail("cannot inspect trial fixture: " + error.message());
  }
  const auto trial = config.trial_index;
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  const auto options = BenchmarkOptions(scenario, config.page_cache_bytes, config.io_mode);

  switch (scenario.workload) {
    case Workload::Portable:
      Fail("portable workload reached TinyDB qualification dispatcher");
    case Workload::Concurrent: {
      PreparePreOpenCacheState(scenario, path);
      const auto memory_before_open = ObserveProcessMemory();
      auto database = Take(Database::Open(DatabasePath(path), options), "Open concurrent trial");
      PrimeEngineReadState(database, scenario, data);
      for (std::size_t round = 0; round < scenario.preparation_rounds; ++round) {
        (void)ObserveConcurrent(database, scenario, data, config.seed ^ round, memory_before_open);
        Check(database.Checkpoint(), "Checkpoint concurrent preparation");
      }
      const auto observation =
          ObserveConcurrent(database, scenario, data, config.seed ^ 0x434F4E435552ULL, memory_before_open);
      const auto residency = ObserveFileResidency(path);
      Check(database.Close(), "Close concurrent trial");
      results.AddTrial(scenario, "reader_throughput", "reads/second", trial, observation.reader_operations_per_second);
      results.AddTrial(scenario, "writer_throughput", "updates/second", trial,
                       observation.writer_operations_per_second);
      AddCommitLatencies(scenario, results, trial, observation.commit_microseconds);
      AddResources(scenario, results, trial, observation.resources, observation.seconds);
      AddCache(scenario, results, trial, observation.cache_resident_bytes, residency);
      return;
    }
    case Workload::Checkpoint:
      AddLifecycleResults(scenario, results, trial, ObserveCheckpoint(path, scenario, data, options), false);
      return;
    case Workload::Recovery:
      AddLifecycleResults(scenario, results, trial, ObserveRecovery(path, scenario, data, options), true);
      return;
    case Workload::Churn:
      RunChurnTrial(path, scenario, data, results, options, trial);
      return;
    case Workload::IoRead: {
      auto random_plan = std::vector<std::size_t>{};
      random_plan.reserve(scenario.operations);
      auto generator = std::mt19937_64{config.seed ^ 0x434F4C44494FULL};
      for (std::size_t operation = 0; operation < scenario.operations; ++operation) {
        random_plan.push_back(static_cast<std::size_t>(generator() % scenario.rows));
      }
      auto expected_scan_digest = std::uint64_t{0};
      if (scenario.access == AccessPattern::Sequential) {
        for (std::size_t row = 0; row < scenario.rows; ++row) {
          expected_scan_digest += data.keys[row].size() * 257U;
          expected_scan_digest += ValueDigest(data.first_values[row]);
        }
      }
      AddIoReadResults(scenario, results, trial,
                       ObserveIoRead(path, scenario, data, options, random_plan, expected_scan_digest));
      return;
    }
  }
  Fail("unknown workload kind");
}

}  // namespace tinydb::bench
