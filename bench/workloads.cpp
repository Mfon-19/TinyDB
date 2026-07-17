#include "benchmark.h"
#include "system_metrics.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tinydb::bench {
namespace {

using Clock = std::chrono::steady_clock;

struct WriteObservation final {
  double operations_per_second{0};
  double wal_amplification{0};
  double persistent_bytes{0};
  std::vector<double> commit_microseconds;
};

enum class MixedValueState : std::uint8_t {
  First,
  Second,
  Missing,
};

struct ReadObservation final {
  double operations_per_second{0};
  double nanoseconds_per_operation{0};
  double cache_hit_rate{0};
};

struct LifecycleObservation final {
  double milliseconds{0};
  double mebibytes_per_second{0};
  double bytes{0};
  double dirty_bytes{0};
  FileResidency residency;
  ProcessIo process_io;
};

struct IoReadObservation final {
  double open_milliseconds{0};
  double workload_milliseconds{0};
  double operations_per_second{0};
  double cache_hit_rate{0};
  double engine_cache_resident_bytes{0};
  bool open_cache_drop_accepted{false};
  bool workload_cache_drop_accepted{false};
  FileResidency before_open_residency;
  FileResidency before_workload_residency;
  FileResidency after_residency;
  ProcessIo open_io;
  ProcessIo workload_io;
  ProcessIo close_io;
  ProcessIo process_io;
  double logical_read_bytes{0};
};

void BuildCheckpointedFixture(const std::filesystem::path &path, const Scenario &scenario, const Dataset &data) {
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open fixture database");
  StoreDataset(database, data, false, scenario);
  Check(database.Checkpoint(), "Checkpoint fixture");
  Check(database.Close(), "Close fixture database");
}

auto MakeOrder(const Scenario &scenario, std::uint64_t seed) -> std::vector<std::size_t> {
  auto order = std::vector<std::size_t>(scenario.rows);
  std::iota(order.begin(), order.end(), 0U);
  if (scenario.random_write_order) {
    auto generator = std::mt19937_64{seed};
    std::ranges::shuffle(order, generator);
  }
  return order;
}

auto ObserveWrite(const Scenario &scenario, const Dataset &data, std::uint64_t seed) -> WriteObservation {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open write benchmark");
  if (scenario.overwrite) {
    StoreDataset(database, data, false, scenario);
    Check(database.Checkpoint(), "Checkpoint overwrite fixture");
  }

  const auto order = MakeOrder(scenario, seed);
  const auto before = Take(database.Stats(), "Stats before writes");
  auto latencies = std::vector<double>{};
  latencies.reserve(scenario.commits);
  const auto started = Clock::now();
  for (std::size_t transaction = 0; transaction < scenario.commits; ++transaction) {
    auto write = Take(database.BeginWrite(), "BeginWrite measured");
    for (std::size_t item = 0; item < scenario.batch; ++item) {
      const auto position = transaction * scenario.batch + item;
      const auto row = order[position];
      const auto &value = scenario.overwrite ? data.second_values[row] : data.first_values[row];
      Check(write.Put(data.keys[row], value), "Put measured");
    }
    const auto commit_started = Clock::now();
    (void)Take(std::move(write).Commit(), "Commit measured");
    latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - commit_started).count());
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after writes");
  {
    auto read = Take(database.BeginRead(), "BeginRead write validation");
    for (std::size_t row = 0; row < data.keys.size(); ++row) {
      const auto value = Take(read.Get(data.keys[row]), "Get written value");
      const auto &expected = scenario.overwrite ? data.second_values[row] : data.first_values[row];
      if (!value || *value != expected) {
        Fail("write validation failed");
      }
    }
  }
  const auto bytes = PersistentBytes(path);
  Check(database.Close(), "Close write benchmark");
  RemoveDatabase(path);

  const auto operations = scenario.commits * scenario.batch;
  const auto wal_delta = after.wal_bytes >= before.wal_bytes ? after.wal_bytes - before.wal_bytes : 0;
  return WriteObservation{
      .operations_per_second = static_cast<double>(operations) / seconds,
      .wal_amplification = static_cast<double>(wal_delta) / static_cast<double>(data.logical_bytes),
      .persistent_bytes = static_cast<double>(bytes),
      .commit_microseconds = std::move(latencies),
  };
}

void RunWrites(const Scenario &scenario, const Config &config, Results &results) {
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveWrite(scenario, data, config.seed ^ warmup);
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveWrite(scenario, data, config.seed ^ (trial + 0x5752495445ULL));
    results.Add(scenario, "throughput", "updates/second", trial, 0, observation.operations_per_second);
    results.Add(scenario, "wal_amplification", "bytes/byte", trial, 0, observation.wal_amplification);
    results.Add(scenario, "persistent_size", "bytes", trial, 0, observation.persistent_bytes);
    for (std::size_t commit = 0; commit < observation.commit_microseconds.size(); ++commit) {
      results.Add(scenario, "commit_latency", "microseconds", trial, commit, observation.commit_microseconds[commit]);
    }
  }
}

struct PointAccess final {
  std::size_t row;
  bool missing;
  Bytes missing_key;
};

auto MakeReadPlan(const Scenario &scenario, std::uint64_t seed) -> std::vector<PointAccess> {
  const auto plan_size = scenario.rows;
  auto plan = std::vector<PointAccess>{};
  plan.reserve(plan_size);
  auto generator = std::mt19937_64{seed};
  const auto hot_rows = std::max<std::size_t>(1, scenario.rows / 10U);
  for (std::size_t operation = 0; operation < plan_size; ++operation) {
    if (scenario.access == AccessPattern::Sequential) {
      const auto row = operation % scenario.rows;
      const auto missing = scenario.include_missing_reads && operation % 2U != 0;
      plan.push_back(PointAccess{row, missing, missing ? MakeKey(scenario.rows + row, scenario.key_bytes) : Bytes{}});
    } else if (scenario.access == AccessPattern::Hotspot && operation % 10U != 0) {
      const auto row = static_cast<std::size_t>(generator() % hot_rows);
      const auto missing = scenario.include_missing_reads && operation % 2U != 0;
      plan.push_back(PointAccess{row, missing, missing ? MakeKey(scenario.rows + row, scenario.key_bytes) : Bytes{}});
    } else {
      const auto row = static_cast<std::size_t>(generator() % scenario.rows);
      const auto missing = scenario.include_missing_reads && operation % 2U != 0;
      plan.push_back(PointAccess{row, missing, missing ? MakeKey(scenario.rows + row, scenario.key_bytes) : Bytes{}});
    }
  }
  return plan;
}

auto ObservePointReads(Database &database, const Scenario &scenario, const Dataset &data,
                       const std::vector<PointAccess> &plan) -> ReadObservation {
  const auto before = Take(database.Stats(), "Stats before point reads");
  auto operations = std::uint64_t{0};
  auto digest = std::uint64_t{0};
  auto expected_digest = std::uint64_t{0};
  auto position = std::size_t{0};

  const auto started = Clock::now();
  const auto read_until_complete = [&](auto &reader) {
    constexpr auto clock_check_interval = std::size_t{256};
    do {
      for (std::size_t operation = 0; operation < clock_check_interval; ++operation) {
        const auto &access = plan[position];
        const auto key = access.missing ? BytesView{access.missing_key} : BytesView{data.keys[access.row]};
        const auto value = Take(reader.Get(key), "Get measured");
        if (access.missing == value.has_value()) {
          Fail("point-read presence validation failed");
        }
        if (value) {
          digest += ValueDigest(*value);
          expected_digest += ValueDigest(data.first_values[access.row]);
        }
        position = (position + 1U) % plan.size();
        operations += 1;
      }
    } while (Clock::now() - started < scenario.minimum_trial);
  };
  if (scenario.transaction_scoped_reads) {
    auto read = Take(database.BeginRead(), "BeginRead point benchmark");
    read_until_complete(read);
  } else {
    read_until_complete(database);
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  if (digest != expected_digest) {
    Fail("point-read value validation failed");
  }

  const auto after = Take(database.Stats(), "Stats after point reads");
  const auto hits = after.cache_hits - before.cache_hits;
  const auto misses = after.cache_misses - before.cache_misses;
  return ReadObservation{
      .operations_per_second = static_cast<double>(operations) / seconds,
      .nanoseconds_per_operation = seconds * 1'000'000'000.0 / static_cast<double>(operations),
      .cache_hit_rate = hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses),
  };
}

void RunPointReads(const Scenario &scenario, const Config &config, Results &results) {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  BuildCheckpointedFixture(path, scenario, data);
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Reopen point benchmark");
  const auto plan = MakeReadPlan(scenario, config.seed ^ 0x52454144ULL);

  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObservePointReads(database, scenario, data, plan);
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObservePointReads(database, scenario, data, plan);
    results.Add(scenario, "throughput", "reads/second", trial, 0, observation.operations_per_second);
    results.Add(scenario, "amortized_latency", "nanoseconds/read", trial, 0, observation.nanoseconds_per_operation);
    results.Add(scenario, "cache_hit_rate", "ratio", trial, 0, observation.cache_hit_rate);
  }
  Check(database.Close(), "Close point benchmark");
  RemoveDatabase(path);
}

struct ScanRange final {
  std::size_t first;
  std::size_t rows;
  std::uint64_t expected_digest;
};

auto MakeScanPlan(const Scenario &scenario, const Dataset &data, std::uint64_t seed) -> std::vector<ScanRange> {
  const auto plan_size = scenario.scan_rows == 0 ? std::size_t{1} : std::size_t{1'024};
  auto plan = std::vector<ScanRange>{};
  plan.reserve(plan_size);
  auto generator = std::mt19937_64{seed};
  const auto rows = scenario.scan_rows == 0 ? scenario.rows : std::min(scenario.scan_rows, scenario.rows);
  for (std::size_t index = 0; index < plan_size; ++index) {
    auto first = std::size_t{0};
    if (rows != scenario.rows) {
      first = index == 0 ? scenario.rows - rows : static_cast<std::size_t>(generator() % (scenario.rows - rows + 1U));
    }
    auto digest = std::uint64_t{0};
    for (std::size_t row = first; row < first + rows; ++row) {
      digest += data.keys[row].size() * 257U;
      digest += scenario.copy_values ? ValueDigest(data.first_values[row]) : data.first_values[row].size();
    }
    plan.push_back(ScanRange{first, rows, digest});
  }
  return plan;
}

auto ObserveScans(Database &database, const Scenario &scenario, const Dataset &data,
                  const std::vector<ScanRange> &plan) -> ReadObservation {
  const auto before = Take(database.Stats(), "Stats before scans");
  auto rows_seen = std::uint64_t{0};
  auto expected_rows = std::uint64_t{0};
  auto digest = std::uint64_t{0};
  auto expected_digest = std::uint64_t{0};
  const auto started = Clock::now();
  do {
    for (const auto &range : plan) {
      auto read = Take(database.BeginRead(), "BeginRead scan");
      auto cursor = [&]() -> Cursor {
        if (range.rows == scenario.rows) {
          return Take(read.Scan(), "Scan full measured");
        }
        if (range.first + range.rows == scenario.rows) {
          return Take(read.Scan(KeyRange::From(data.keys[range.first])), "Scan final range measured");
        }
        return Take(read.Scan(KeyRange::Between(data.keys[range.first], data.keys[range.first + range.rows])),
                    "Scan range measured");
      }();
      while (cursor.Valid()) {
        digest += cursor.Key().size() * 257U;
        digest += scenario.copy_values ? ValueDigest(Take(cursor.CopyValue(), "Copy scan value")) : cursor.ValueSize();
        rows_seen += 1;
        Check(cursor.Next(), "Cursor Next measured");
      }
      expected_rows += range.rows;
      expected_digest += range.expected_digest;
    }
  } while (Clock::now() - started < scenario.minimum_trial);
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  if (rows_seen != expected_rows || digest != expected_digest) {
    Fail("scan validation failed");
  }
  const auto after = Take(database.Stats(), "Stats after scans");
  const auto hits = after.cache_hits - before.cache_hits;
  const auto misses = after.cache_misses - before.cache_misses;
  return ReadObservation{
      .operations_per_second = static_cast<double>(rows_seen) / seconds,
      .nanoseconds_per_operation = seconds * 1'000'000'000.0 / static_cast<double>(rows_seen),
      .cache_hit_rate = hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses),
  };
}

void RunScans(const Scenario &scenario, const Config &config, Results &results) {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  BuildCheckpointedFixture(path, scenario, data);
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Reopen scan benchmark");
  const auto plan = MakeScanPlan(scenario, data, config.seed ^ 0x5343414EULL);
  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveScans(database, scenario, data, plan);
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveScans(database, scenario, data, plan);
    results.Add(scenario, "throughput", "rows/second", trial, 0, observation.operations_per_second);
    results.Add(scenario, "amortized_latency", "nanoseconds/row", trial, 0, observation.nanoseconds_per_operation);
    results.Add(scenario, "cache_hit_rate", "ratio", trial, 0, observation.cache_hit_rate);
  }
  Check(database.Close(), "Close scan benchmark");
  RemoveDatabase(path);
}

auto MixedRow(const Scenario &scenario, std::mt19937_64 &generator) -> std::size_t {
  if (scenario.access == AccessPattern::Hotspot && generator() % 10U != 0) {
    return static_cast<std::size_t>(generator() % std::max<std::size_t>(1, scenario.rows / 10U));
  }
  return static_cast<std::size_t>(generator() % scenario.rows);
}

auto ObserveMixed(Database &database, const Scenario &scenario, const Dataset &data,
                  std::vector<MixedValueState> &states, std::size_t generation,
                  std::uint64_t seed) -> WriteObservation {
  auto generator = std::mt19937_64{seed};
  const auto before = Take(database.Stats(), "Stats before mixed workload");
  auto latencies = std::vector<double>{};
  latencies.reserve(scenario.commits);
  auto logical_write_bytes = std::uint64_t{0};
  const auto started = Clock::now();
  for (std::size_t transaction = 0; transaction < scenario.commits; ++transaction) {
    auto write = Take(database.BeginWrite(), "BeginWrite mixed");
    for (std::size_t operation = 0; operation < scenario.batch; ++operation) {
      const auto selector = operation % 25U;
      const auto row = MixedRow(scenario, generator);
      if (selector < 20U) {
        const auto value = Take(write.Get(data.keys[row]), "Get mixed");
        if (states[row] == MixedValueState::Missing) {
          if (value) {
            Fail("mixed read found a deleted key");
          }
        } else {
          if (!value) {
            Fail("mixed read missed a present key");
          }
          const auto &expected =
              states[row] == MixedValueState::First ? data.first_values[row] : data.second_values[row];
          if (*value != expected) {
            Fail("mixed read returned the wrong value");
          }
        }
      } else if (selector < 23U) {
        Check(write.Put(data.keys[row], data.second_values[row]), "Update mixed");
        states[row] = MixedValueState::Second;
        logical_write_bytes += data.keys[row].size() + data.second_values[row].size();
      } else if (selector == 23U) {
        const auto insert_row =
            scenario.rows + generation * scenario.commits * scenario.batch + transaction * scenario.batch + operation;
        const auto key = MakeKey(insert_row, scenario.key_bytes);
        const auto value = MakeValue(insert_row, scenario.value_bytes, 0);
        Check(write.Put(key, value), "Insert mixed");
        logical_write_bytes += key.size() + value.size();
      } else {
        Check(write.Delete(data.keys[row]), "Delete mixed");
        states[row] = MixedValueState::Missing;
        logical_write_bytes += data.keys[row].size();
      }
    }
    const auto commit_started = Clock::now();
    (void)Take(std::move(write).Commit(), "Commit mixed");
    latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - commit_started).count());
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after mixed workload");
  const auto wal_delta = after.wal_bytes >= before.wal_bytes ? after.wal_bytes - before.wal_bytes : 0;
  return WriteObservation{
      .operations_per_second = static_cast<double>(scenario.commits * scenario.batch) / seconds,
      .wal_amplification =
          logical_write_bytes == 0 ? 0.0 : static_cast<double>(wal_delta) / static_cast<double>(logical_write_bytes),
      .persistent_bytes = 0,
      .commit_microseconds = std::move(latencies),
  };
}

void RunMixed(const Scenario &scenario, const Config &config, Results &results) {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  BuildCheckpointedFixture(path, scenario, data);
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Reopen mixed benchmark");
  auto states = std::vector<MixedValueState>(scenario.rows, MixedValueState::First);
  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveMixed(database, scenario, data, states, warmup, config.seed ^ (warmup + 0x4D49584544ULL));
    Check(database.Checkpoint(), "Checkpoint after mixed warmup");
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveMixed(database, scenario, data, states, scenario.warmups + trial,
                                          config.seed ^ (trial + 0x4D49584544ULL));
    results.Add(scenario, "throughput", "operations/second", trial, 0, observation.operations_per_second);
    results.Add(scenario, "wal_amplification", "bytes/byte", trial, 0, observation.wal_amplification);
    for (std::size_t commit = 0; commit < observation.commit_microseconds.size(); ++commit) {
      results.Add(scenario, "commit_latency", "microseconds", trial, commit, observation.commit_microseconds[commit]);
    }
    Check(database.Checkpoint(), "Checkpoint after mixed trial");
  }
  Check(database.Close(), "Close mixed benchmark");
  RemoveDatabase(path);
}

struct ConcurrentObservation final {
  double reader_operations_per_second{0};
  double writer_operations_per_second{0};
  std::vector<double> commit_microseconds;
};

auto ObserveConcurrent(Database &database, const Scenario &scenario, const Dataset &data,
                       std::uint64_t seed) -> ConcurrentObservation {
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
  const auto read_operations = std::accumulate(reader_counts.begin(), reader_counts.end(), std::uint64_t{0});
  return ConcurrentObservation{
      .reader_operations_per_second = static_cast<double>(read_operations) / seconds,
      .writer_operations_per_second = static_cast<double>(scenario.commits * scenario.batch) / seconds,
      .commit_microseconds = std::move(latencies),
  };
}

void RunConcurrent(const Scenario &scenario, const Config &config, Results &results) {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  BuildCheckpointedFixture(path, scenario, data);
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Reopen concurrency benchmark");
  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveConcurrent(database, scenario, data, config.seed ^ warmup);
    Check(database.Checkpoint(), "Checkpoint after concurrency warmup");
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveConcurrent(database, scenario, data, config.seed ^ (trial + 0x434F4E435552ULL));
    results.Add(scenario, "reader_throughput", "reads/second", trial, 0, observation.reader_operations_per_second);
    results.Add(scenario, "writer_throughput", "updates/second", trial, 0, observation.writer_operations_per_second);
    for (std::size_t commit = 0; commit < observation.commit_microseconds.size(); ++commit) {
      results.Add(scenario, "commit_latency", "microseconds", trial, commit, observation.commit_microseconds[commit]);
    }
    Check(database.Checkpoint(), "Checkpoint after concurrency trial");
  }
  Check(database.Close(), "Close concurrency benchmark");
  RemoveDatabase(path);
}

auto ObserveCheckpoint(const Scenario &scenario, const Dataset &data) -> LifecycleObservation {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open checkpoint benchmark");
  StoreDataset(database, data, false, scenario);
  const auto before = Take(database.Stats(), "Stats before checkpoint");
  if (before.dirty_bytes == 0) {
    Fail("checkpoint fixture produced no dirty bytes");
  }
  const auto process_io_before = ObserveProcessIo();
  const auto started = Clock::now();
  Check(database.Checkpoint(), "Checkpoint measured");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto after = Take(database.Stats(), "Stats after checkpoint");
  if (after.dirty_pages != 0) {
    Fail("checkpoint left dirty pages");
  }
  const auto first = Take(database.Get(data.keys.front()), "Get first checkpointed key");
  const auto last = Take(database.Get(data.keys.back()), "Get last checkpointed key");
  if (!first || !last || *first != data.first_values.front() || *last != data.first_values.back()) {
    Fail("checkpoint validation failed");
  }
  const auto bytes = DatabaseFileBytes(path);
  Check(database.Close(), "Close checkpoint benchmark");
  const auto process_io = SubtractProcessIo(ObserveProcessIo(), process_io_before);
  const auto residency = ObserveFileResidency(path);
  RemoveDatabase(path);
  return LifecycleObservation{
      .milliseconds = seconds * 1'000.0,
      .mebibytes_per_second = (static_cast<double>(before.dirty_bytes) / static_cast<double>(1U << 20U)) / seconds,
      .bytes = static_cast<double>(bytes),
      .dirty_bytes = static_cast<double>(before.dirty_bytes),
      .residency = residency,
      .process_io = process_io,
  };
}

void RunCheckpoint(const Scenario &scenario, Results &results) {
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveCheckpoint(scenario, data);
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveCheckpoint(scenario, data);
    results.Add(scenario, "latency", "milliseconds", trial, 0, observation.milliseconds);
    results.Add(scenario, "dirty_transfer_rate", "MiB/second", trial, 0, observation.mebibytes_per_second);
    results.Add(scenario, "database_size", "bytes", trial, 0, observation.bytes);
    results.Add(scenario, "page_cache_resident_bytes", "bytes", trial, 0,
                static_cast<double>(observation.residency.resident_bytes));
    results.Add(scenario, "page_cache_resident_ratio", "ratio", trial, 0, observation.residency.Ratio());
    results.Add(scenario, "storage_write_bytes", "bytes", trial, 0,
                static_cast<double>(observation.process_io.storage_write_bytes));
    results.Add(scenario, "storage_write_amplification", "bytes/dirty_byte", trial, 0,
                observation.dirty_bytes == 0
                    ? 0.0
                    : static_cast<double>(observation.process_io.storage_write_bytes) / observation.dirty_bytes);
    results.Add(scenario, "write_syscalls", "calls", trial, 0,
                static_cast<double>(observation.process_io.write_syscalls));
  }
}

[[noreturn]] void RecoveryWriter(const std::filesystem::path &path, const Dataset &data, const Scenario &scenario) {
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open recovery writer");
  StoreDataset(database, data, false, scenario);
  ::_exit(0);
}

void WaitForWriter(pid_t child) {
  auto status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    Fail("recovery writer child failed");
  }
}

auto ObserveRecovery(const Scenario &scenario, const Dataset &data) -> LifecycleObservation {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  const auto child = ::fork();
  if (child == 0) {
    RecoveryWriter(path, data, scenario);
  }
  if (child < 0) {
    Fail("fork failed");
  }
  WaitForWriter(child);
  const auto bytes = PersistentBytes(path);
  const auto process_io_before = ObserveProcessIo();
  const auto started = Clock::now();
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open measured recovery");
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  const auto first = Take(database.Get(data.keys.front()), "Get first recovered key");
  const auto last = Take(database.Get(data.keys.back()), "Get last recovered key");
  if (!first || !last || *first != data.first_values.front() || *last != data.first_values.back()) {
    Fail("recovery validation failed");
  }
  Check(database.Close(), "Close recovery benchmark");
  const auto process_io = SubtractProcessIo(ObserveProcessIo(), process_io_before);
  const auto residency = ObserveFileResidency(path);
  RemoveDatabase(path);
  return LifecycleObservation{
      .milliseconds = seconds * 1'000.0,
      .mebibytes_per_second = (static_cast<double>(bytes) / static_cast<double>(1U << 20U)) / seconds,
      .bytes = static_cast<double>(bytes),
      .dirty_bytes = 0,
      .residency = residency,
      .process_io = process_io,
  };
}

void RunRecovery(const Scenario &scenario, Results &results) {
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveRecovery(scenario, data);
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveRecovery(scenario, data);
    results.Add(scenario, "open_latency", "milliseconds", trial, 0, observation.milliseconds);
    results.Add(scenario, "replay_rate", "MiB/second", trial, 0, observation.mebibytes_per_second);
    results.Add(scenario, "persistent_size", "bytes", trial, 0, observation.bytes);
    results.Add(scenario, "page_cache_resident_bytes", "bytes", trial, 0,
                static_cast<double>(observation.residency.resident_bytes));
    results.Add(scenario, "page_cache_resident_ratio", "ratio", trial, 0, observation.residency.Ratio());
    results.Add(scenario, "storage_read_bytes", "bytes", trial, 0,
                static_cast<double>(observation.process_io.storage_read_bytes));
    results.Add(scenario, "storage_write_bytes", "bytes", trial, 0,
                static_cast<double>(observation.process_io.storage_write_bytes));
  }
}

auto ObserveIoRead(const std::filesystem::path &path, const Scenario &scenario, const Dataset &data,
                   const std::vector<std::size_t> &random_plan,
                   std::uint64_t expected_scan_digest) -> IoReadObservation {
  const auto open_cache_drop_accepted = !scenario.drop_file_cache || AdviseDropFileCache(path);
  const auto before_open_residency = ObserveFileResidency(path);
  const auto process_io_before = ObserveProcessIo();

  const auto open_started = Clock::now();
  auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open direct-I/O read benchmark");
  const auto open_seconds = std::chrono::duration<double>(Clock::now() - open_started).count();
  const auto process_io_after_open = ObserveProcessIo();
  const auto stats_before = Take(database.Stats(), "Stats before direct-I/O read");

  /*
  ** Open performs real database work and may populate the file cache.  Evict a
  ** second time so open cost and cold workload cost remain separately visible.
  ** The database is quiescent and the advisory descriptor never reads or
  ** writes file contents.
  */
  const auto workload_cache_drop_accepted = !scenario.drop_file_cache || AdviseDropFileCache(path);
  const auto before_workload_residency = ObserveFileResidency(path);
  const auto process_io_before_workload = ObserveProcessIo();

  auto operations = std::uint64_t{0};
  auto digest = std::uint64_t{0};
  const auto workload_started = Clock::now();
  if (scenario.access == AccessPattern::Sequential) {
    auto read = Take(database.BeginRead(), "BeginRead direct-I/O scan");
    auto cursor = Take(read.Scan(), "Scan direct-I/O fixture");
    while (cursor.Valid()) {
      digest += cursor.Key().size() * 257U;
      digest += ValueDigest(Take(cursor.CopyValue(), "Copy direct-I/O scan value"));
      ++operations;
      Check(cursor.Next(), "Advance direct-I/O scan");
    }
    if (operations != scenario.rows || digest != expected_scan_digest) {
      Fail("direct-I/O scan validation failed");
    }
  } else {
    auto read = Take(database.BeginRead(), "BeginRead direct-I/O random reads");
    for (const auto row : random_plan) {
      const auto value = Take(read.Get(data.keys[row]), "Get direct-I/O random key");
      if (!value || *value != data.first_values[row]) {
        Fail("direct-I/O random-read validation failed");
      }
      digest += ValueDigest(*value);
      ++operations;
    }
    if (operations != random_plan.size() || digest == 0) {
      Fail("direct-I/O random-read plan was not fully executed");
    }
  }
  const auto workload_seconds = std::chrono::duration<double>(Clock::now() - workload_started).count();
  const auto process_io_after_workload = ObserveProcessIo();
  const auto stats_after = Take(database.Stats(), "Stats after direct-I/O read");
  Check(database.Close(), "Close direct-I/O read benchmark");

  const auto process_io_after_close = ObserveProcessIo();
  const auto process_io = SubtractProcessIo(process_io_after_close, process_io_before);
  const auto after_residency = ObserveFileResidency(path);
  const auto hits = stats_after.cache_hits - stats_before.cache_hits;
  const auto misses = stats_after.cache_misses - stats_before.cache_misses;
  return IoReadObservation{
      .open_milliseconds = open_seconds * 1'000.0,
      .workload_milliseconds = workload_seconds * 1'000.0,
      .operations_per_second = static_cast<double>(operations) / workload_seconds,
      .cache_hit_rate = hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses),
      .engine_cache_resident_bytes = static_cast<double>(stats_after.cache_resident_bytes),
      .open_cache_drop_accepted = open_cache_drop_accepted,
      .workload_cache_drop_accepted = workload_cache_drop_accepted,
      .before_open_residency = before_open_residency,
      .before_workload_residency = before_workload_residency,
      .after_residency = after_residency,
      .open_io = SubtractProcessIo(process_io_after_open, process_io_before),
      .workload_io = SubtractProcessIo(process_io_after_workload, process_io_before_workload),
      .close_io = SubtractProcessIo(process_io_after_close, process_io_after_workload),
      .process_io = process_io,
      .logical_read_bytes =
          static_cast<double>(operations) * static_cast<double>(scenario.key_bytes + scenario.value_bytes),
  };
}

void AddIoReadResults(const Scenario &scenario, Results &results, std::size_t trial,
                      const IoReadObservation &observation) {
  results.Add(scenario, "open_latency", "milliseconds", trial, 0, observation.open_milliseconds);
  results.Add(scenario, "workload_latency", "milliseconds", trial, 0, observation.workload_milliseconds);
  results.Add(scenario, "throughput", scenario.access == AccessPattern::Sequential ? "rows/second" : "reads/second",
              trial, 0, observation.operations_per_second);
  results.Add(scenario, "cache_hit_rate", "ratio", trial, 0, observation.cache_hit_rate);
  results.Add(scenario, "open_cache_drop_accepted", "boolean", trial, 0,
              observation.open_cache_drop_accepted ? 1.0 : 0.0);
  results.Add(scenario, "workload_cache_drop_accepted", "boolean", trial, 0,
              observation.workload_cache_drop_accepted ? 1.0 : 0.0);
  results.Add(scenario, "page_cache_pre_open_resident_bytes", "bytes", trial, 0,
              static_cast<double>(observation.before_open_residency.resident_bytes));
  results.Add(scenario, "page_cache_pre_open_resident_ratio", "ratio", trial, 0,
              observation.before_open_residency.Ratio());
  results.Add(scenario, "page_cache_pre_workload_resident_bytes", "bytes", trial, 0,
              static_cast<double>(observation.before_workload_residency.resident_bytes));
  results.Add(scenario, "page_cache_pre_workload_resident_ratio", "ratio", trial, 0,
              observation.before_workload_residency.Ratio());
  results.Add(scenario, "page_cache_post_resident_bytes", "bytes", trial, 0,
              static_cast<double>(observation.after_residency.resident_bytes));
  results.Add(scenario, "page_cache_post_resident_ratio", "ratio", trial, 0, observation.after_residency.Ratio());
  results.Add(scenario, "page_cache_growth", "bytes", trial, 0,
              static_cast<double>(observation.after_residency.resident_bytes) -
                  static_cast<double>(observation.before_workload_residency.resident_bytes));
  results.Add(scenario, "engine_cache_resident_bytes", "bytes", trial, 0, observation.engine_cache_resident_bytes);
  results.Add(
      scenario, "combined_cache_resident_bytes", "bytes", trial, 0,
      observation.engine_cache_resident_bytes + static_cast<double>(observation.after_residency.resident_bytes));
  results.Add(scenario, "storage_read_bytes", "bytes", trial, 0,
              static_cast<double>(observation.process_io.storage_read_bytes));
  results.Add(scenario, "open_storage_read_bytes", "bytes", trial, 0,
              static_cast<double>(observation.open_io.storage_read_bytes));
  results.Add(scenario, "workload_storage_read_bytes", "bytes", trial, 0,
              static_cast<double>(observation.workload_io.storage_read_bytes));
  results.Add(scenario, "close_storage_read_bytes", "bytes", trial, 0,
              static_cast<double>(observation.close_io.storage_read_bytes));
  results.Add(scenario, "workload_storage_read_amplification", "bytes/logical_byte", trial, 0,
              observation.logical_read_bytes == 0
                  ? 0.0
                  : static_cast<double>(observation.workload_io.storage_read_bytes) / observation.logical_read_bytes);
  results.Add(scenario, "read_characters", "bytes", trial, 0,
              static_cast<double>(observation.process_io.characters_read));
  results.Add(scenario, "read_syscalls", "calls", trial, 0, static_cast<double>(observation.process_io.read_syscalls));
}

void RunIoRead(const Scenario &scenario, const Config &config, Results &results) {
  const auto path = TemporaryDatabasePath(scenario.name);
  RemoveDatabase(path);
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  BuildCheckpointedFixture(path, scenario, data);

  auto random_plan = std::vector<std::size_t>{};
  random_plan.reserve(scenario.operations);
  auto generator = std::mt19937_64{config.seed ^ 0x444952454354494FULL};
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

  for (std::size_t warmup = 0; warmup < scenario.warmups; ++warmup) {
    (void)ObserveIoRead(path, scenario, data, random_plan, expected_scan_digest);
  }
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto observation = ObserveIoRead(path, scenario, data, random_plan, expected_scan_digest);
    AddIoReadResults(scenario, results, trial, observation);
  }
  RemoveDatabase(path);
}

auto ChurnRound(Database &database, const Scenario &scenario, const Dataset &data, bool second) -> double {
  const auto started = Clock::now();
  DeleteDataset(database, data);
  StoreDataset(database, data, second, scenario);
  Check(database.Checkpoint(), "Checkpoint churn round");
  return std::chrono::duration<double>(Clock::now() - started).count();
}

void RunChurn(const Scenario &scenario, Results &results) {
  const auto data = MakeDataset(scenario.rows, scenario.key_bytes, scenario.value_bytes);
  for (std::size_t trial = 0; trial < scenario.trials; ++trial) {
    const auto path = TemporaryDatabasePath(scenario.name);
    RemoveDatabase(path);
    auto database = Take(Database::Open(path, BenchmarkOptions(scenario)), "Open churn benchmark");
    StoreDataset(database, data, false, scenario);
    Check(database.Checkpoint(), "Checkpoint churn fixture");
    for (std::size_t round = 0; round < scenario.churn_warmup_rounds; ++round) {
      (void)ChurnRound(database, scenario, data, round % 2U == 0);
    }

    auto first_bytes = std::uint64_t{0};
    auto last_bytes = std::uint64_t{0};
    for (std::size_t round = 0; round < scenario.churn_measured_rounds; ++round) {
      const auto seconds = ChurnRound(database, scenario, data, round % 2U != 0);
      const auto bytes = DatabaseFileBytes(path);
      if (round == 0) {
        first_bytes = bytes;
      }
      last_bytes = bytes;
      results.Add(scenario, "throughput", "operations/second", trial, round,
                  static_cast<double>(scenario.rows * 2U) / seconds);
      results.Add(scenario, "file_amplification", "bytes/byte", trial, round,
                  static_cast<double>(bytes) / static_cast<double>(data.logical_bytes));
      results.Add(scenario, "database_size", "bytes", trial, round, static_cast<double>(bytes));
    }
    const auto value = Take(database.Get(data.keys.back()), "Get final churn key");
    const auto final_uses_second = (scenario.churn_measured_rounds - 1U) % 2U != 0;
    const auto &expected = final_uses_second ? data.second_values.back() : data.first_values.back();
    if (!value || *value != expected) {
      Fail("churn validation failed");
    }
    results.Add(scenario, "growth", "bytes/round", trial, 0,
                (static_cast<double>(last_bytes) - static_cast<double>(first_bytes)) /
                    static_cast<double>(std::max<std::size_t>(1, scenario.churn_measured_rounds - 1U)));
    Check(database.Close(), "Close churn benchmark");
    RemoveDatabase(path);
  }
}

}  // namespace

void RunScenario(const Scenario &scenario, const Config &config, Results &results) {
  switch (scenario.workload) {
    case Workload::Put:
      RunWrites(scenario, config, results);
      return;
    case Workload::PointRead:
      RunPointReads(scenario, config, results);
      return;
    case Workload::Scan:
      RunScans(scenario, config, results);
      return;
    case Workload::Mixed:
      RunMixed(scenario, config, results);
      return;
    case Workload::Concurrent:
      RunConcurrent(scenario, config, results);
      return;
    case Workload::Checkpoint:
      RunCheckpoint(scenario, results);
      return;
    case Workload::Recovery:
      RunRecovery(scenario, results);
      return;
    case Workload::Churn:
      RunChurn(scenario, results);
      return;
    case Workload::IoRead:
      RunIoRead(scenario, config, results);
      return;
  }
  Fail("unknown workload kind");
}

}  // namespace tinydb::bench
