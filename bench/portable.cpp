#include "benchmark.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydb::bench {
namespace {

using Clock = std::chrono::steady_clock;

struct Profile final {
  std::size_t write_records;
  std::size_t read_records;
  std::size_t read_operations;
  std::size_t ycsb_records;
  std::size_t ycsb_operations;
  std::size_t trials;
};

struct PortableData final {
  std::vector<std::string> keys;
  std::string first_value;
  std::string second_value;
};

struct Observation final {
  std::uint64_t logical_operations{0};
  std::uint64_t logical_read_bytes{0};
  std::uint64_t logical_write_bytes{0};
  std::uint64_t digest{0};
  double seconds{0};
  std::vector<double> call_latencies;
};

enum class YcsbOperation {
  Read,
  Update,
  Insert,
  Scan,
  ReadModifyWrite,
};

struct YcsbAction final {
  YcsbOperation operation{YcsbOperation::Read};
  std::size_t key{0};
  std::size_t scan_rows{0};
};

auto SelectedProfile(std::string_view name) -> Profile {
  if (name == "smoke") {
    return {2'000, 2'000, 5'000, 2'000, 2'000, 1};
  }
  if (name == "standard") {
    return {20'000, 250'000, 250'000, 50'000, 50'000, 3};
  }
  if (name == "soak") {
    return {200'000, 2'000'000, 2'000'000, 500'000, 1'000'000, 5};
  }
  Fail("unknown benchmark profile");
}

auto EstimateBytes(std::size_t rows, std::size_t key_bytes, std::size_t value_bytes) -> std::uint64_t {
  constexpr auto overhead = std::uint64_t{32};
  return static_cast<std::uint64_t>(rows) * (static_cast<std::uint64_t>(key_bytes) + value_bytes + overhead);
}

auto BasePortable(std::string name, std::string family, std::string portable_workload, std::size_t rows,
                  std::size_t operations, std::size_t value_bytes, std::size_t trials) -> Scenario {
  auto scenario = Scenario{};
  scenario.name = std::move(name);
  scenario.family = std::move(family);
  scenario.workload = Workload::Portable;
  scenario.portable_workload = std::move(portable_workload);
  scenario.primary_metric = "throughput";
  scenario.primary_direction = MetricDirection::Higher;
  scenario.meaningful_difference = 0.05;
  scenario.rows = rows;
  scenario.operations = operations;
  scenario.value_bytes = value_bytes;
  scenario.trials = trials;
  scenario.target_bytes = EstimateBytes(rows, scenario.key_bytes, value_bytes);
  return scenario;
}

auto Mix64(std::uint64_t value) -> std::uint64_t {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

auto MakePortableKey(std::size_t row, std::size_t bytes, bool hashed) -> std::string {
  const auto id = hashed ? Mix64(row) : static_cast<std::uint64_t>(row);
  auto encoded = std::array<char, 16>{};
  const auto conversion = std::to_chars(encoded.data(), encoded.data() + encoded.size(), id, 16);
  if (conversion.ec != std::errc{}) {
    Fail("portable key encoding failed");
  }
  const auto length = static_cast<std::size_t>(conversion.ptr - encoded.data());
  auto key = std::string(bytes, '0');
  std::copy(encoded.data(), conversion.ptr, key.begin() + static_cast<std::ptrdiff_t>(16U - length));
  for (auto index = std::size_t{16}; index < key.size(); ++index) {
    key[index] = static_cast<char>('a' + (row + index) % 26U);
  }
  return key;
}

auto MakePortableValue(std::size_t bytes, std::uint64_t salt) -> std::string {
  auto value = std::string(bytes, 'v');
  for (auto index = std::size_t{0}; index < std::min<std::size_t>(bytes, 16); ++index) {
    value[index] = static_cast<char>('!' + ((salt >> ((index % 8U) * 8U)) + index) % 90U);
  }
  return value;
}

auto MakeData(const Scenario &scenario) -> PortableData {
  const auto ycsb = scenario.family == "ycsb";
  const auto extra = ycsb ? scenario.operations : 0;
  auto data = PortableData{};
  data.keys.reserve(scenario.rows + extra);
  for (auto row = std::size_t{0}; row < scenario.rows + extra; ++row) {
    data.keys.push_back(MakePortableKey(row, scenario.key_bytes, ycsb));
  }
  data.first_value = MakePortableValue(scenario.value_bytes, 0x12345678U);
  data.second_value = MakePortableValue(scenario.value_bytes, 0xA55A5AA5U);
  return data;
}

auto IsFill(std::string_view workload) -> bool {
  return workload == "fillseq" || workload == "fillrandom" || workload == "fillbatch" || workload == "filllarge";
}

void Populate(Backend &backend, const Scenario &scenario, const PortableData &data) {
  constexpr auto maximum_batch = std::size_t{256};
  const auto bytes_per_entry = std::max<std::size_t>(1, scenario.key_bytes + scenario.value_bytes);
  const auto batch = std::max<std::size_t>(1, std::min(maximum_batch, (4U << 20U) / bytes_per_entry));
  auto entries = std::vector<Entry>{};
  entries.reserve(batch);
  for (auto row = std::size_t{0}; row < scenario.rows; ++row) {
    entries.push_back({data.keys[row], data.first_value});
    if (entries.size() == batch || row + 1U == scenario.rows) {
      backend.Put(entries);
      entries.clear();
    }
  }
}

void ValidateFixture(Backend &backend, const Scenario &scenario, const PortableData &data) {
  if (scenario.rows == 0) {
    return;
  }
  const auto first = backend.Get(data.keys.front());
  const auto last = backend.Get(data.keys[scenario.rows - 1U]);
  if (!first || !last || *first != data.first_value || *last != data.first_value) {
    Fail("portable fixture validation failed");
  }
}

void PrimeReads(Backend &backend, const Scenario &scenario) {
  if (IsFill(scenario.portable_workload)) {
    return;
  }
  const auto scan = backend.Scan(std::nullopt, scenario.rows);
  if (scan.rows != scenario.rows) {
    Fail("portable cache priming did not visit the complete fixture");
  }
}

template <typename Function>
void ObserveCall(Observation &observation, Function &&function) {
  const auto started = Clock::now();
  function();
  observation.call_latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
}

auto Percentile(std::vector<double> values, double fraction) -> double {
  if (values.empty()) {
    Fail("portable workload produced no latency observations");
  }
  std::ranges::sort(values);
  const auto position = fraction * static_cast<double>(values.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const auto weight = position - static_cast<double>(lower);
  return values[lower] * (1.0 - weight) + values[upper] * weight;
}

auto MakeDbPlan(const Scenario &scenario, std::uint64_t seed) -> std::vector<std::size_t> {
  auto plan = std::vector<std::size_t>(scenario.operations);
  std::iota(plan.begin(), plan.end(), 0U);
  auto generator = std::mt19937_64{seed};
  if (scenario.portable_workload == "fillrandom" || scenario.portable_workload == "filllarge" ||
      scenario.portable_workload == "deleterandom") {
    std::ranges::shuffle(plan, generator);
  } else if (scenario.portable_workload == "readrandom" || scenario.portable_workload == "readrange" ||
             scenario.portable_workload == "seekrandom" || scenario.portable_workload == "overwrite") {
    for (auto &row : plan) {
      row = static_cast<std::size_t>(generator() % scenario.rows);
    }
  }
  return plan;
}

auto RunDbWorkload(Backend &backend, const Scenario &scenario, const PortableData &data,
                   const std::vector<std::size_t> &plan) -> Observation {
  auto observation = Observation{};
  observation.call_latencies.reserve(std::min<std::size_t>(scenario.operations, 20'000));
  const auto started = Clock::now();
  const auto &workload = scenario.portable_workload;

  if (IsFill(workload) || workload == "overwrite") {
    const auto batch = std::max<std::size_t>(1, scenario.batch);
    auto entries = std::vector<Entry>{};
    entries.reserve(batch);
    for (auto operation = std::size_t{0}; operation < scenario.operations;) {
      const auto count = std::min(batch, scenario.operations - operation);
      entries.clear();
      for (auto offset = std::size_t{0}; offset < count; ++offset) {
        const auto row = plan[operation + offset];
        entries.push_back({data.keys[row], workload == "overwrite" ? data.second_value : data.first_value});
      }
      ObserveCall(observation, [&] { backend.Put(entries); });
      observation.logical_operations += count;
      observation.logical_write_bytes += count * (scenario.key_bytes + scenario.value_bytes);
      operation += count;
    }
  } else if (workload == "readrandom") {
    for (const auto row : plan) {
      ObserveCall(observation, [&] {
        const auto value = backend.Get(data.keys[row]);
        if (!value || *value != data.first_value) {
          Fail("readrandom missed or corrupted an existing value");
        }
        observation.digest ^= Mix64(value->size() + row);
      });
      ++observation.logical_operations;
      observation.logical_read_bytes += scenario.key_bytes + scenario.value_bytes;
    }
  } else if (workload == "readseq") {
    auto remaining = scenario.operations;
    auto position = std::size_t{0};
    constexpr auto scan_batch = std::size_t{4'096};
    while (remaining != 0) {
      const auto rows = std::min({remaining, scan_batch, scenario.rows - position});
      ObserveCall(observation, [&] {
        const auto scan = backend.Scan(
            position == 0 ? std::optional<std::string_view>{} : std::optional<std::string_view>{data.keys[position]},
            rows);
        if (scan.rows != rows) {
          Fail("readseq ended before its requested row count");
        }
        observation.digest ^= scan.digest;
      });
      observation.logical_operations += rows;
      observation.logical_read_bytes += rows * (scenario.key_bytes + scenario.value_bytes);
      remaining -= rows;
      position = (position + rows) % scenario.rows;
    }
  } else if (workload == "readrange") {
    for (const auto row : plan) {
      ObserveCall(observation, [&] {
        const auto scan = backend.Scan(data.keys[row], std::min(scenario.scan_rows, scenario.rows - row));
        if (scan.rows == 0) {
          Fail("readrange returned no rows");
        }
        observation.logical_operations += scan.rows;
        observation.logical_read_bytes += scan.rows * (scenario.key_bytes + scenario.value_bytes);
        observation.digest ^= scan.digest;
      });
    }
  } else if (workload == "seekrandom") {
    for (const auto row : plan) {
      ObserveCall(observation, [&] {
        const auto scan = backend.Scan(data.keys[row], 1);
        if (scan.rows != 1) {
          Fail("seekrandom missed an existing key");
        }
        observation.digest ^= scan.digest;
      });
      ++observation.logical_operations;
      observation.logical_read_bytes += scenario.key_bytes + scenario.value_bytes;
    }
  } else if (workload == "deleterandom") {
    for (const auto row : plan) {
      ObserveCall(observation, [&] { backend.Delete(data.keys[row]); });
      ++observation.logical_operations;
      observation.logical_write_bytes += scenario.key_bytes;
    }
    for (const auto row : plan) {
      if (backend.Get(data.keys[row])) {
        Fail("deleterandom left a deleted key present");
      }
    }
  } else {
    Fail("unknown portable db_bench workload");
  }
  observation.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return observation;
}

auto MakeZipfWeights(std::size_t rows) -> std::discrete_distribution<std::size_t> {
  auto weights = std::vector<double>{};
  weights.reserve(rows);
  for (auto row = std::size_t{0}; row < rows; ++row) {
    weights.push_back(1.0 / std::pow(static_cast<double>(row + 1U), 0.99));
  }
  return std::discrete_distribution<std::size_t>{weights.begin(), weights.end()};
}

auto MakeYcsbPlan(const Scenario &scenario, std::uint64_t seed) -> std::vector<YcsbAction> {
  auto generator = std::mt19937_64{seed};
  auto zipf = MakeZipfWeights(scenario.rows);
  auto actions = std::vector<YcsbAction>{};
  actions.reserve(scenario.operations);
  auto inserted = std::size_t{0};
  for (auto operation = std::size_t{0}; operation < scenario.operations; ++operation) {
    const auto choice = static_cast<unsigned>(generator() % 100U);
    auto action = YcsbAction{};
    const auto &name = scenario.portable_workload;
    if (name == "ycsb_a") {
      action.operation = choice < 50U ? YcsbOperation::Read : YcsbOperation::Update;
    } else if (name == "ycsb_b") {
      action.operation = choice < 95U ? YcsbOperation::Read : YcsbOperation::Update;
    } else if (name == "ycsb_c") {
      action.operation = YcsbOperation::Read;
    } else if (name == "ycsb_d") {
      action.operation = choice < 95U ? YcsbOperation::Read : YcsbOperation::Insert;
    } else if (name == "ycsb_e") {
      action.operation = choice < 95U ? YcsbOperation::Scan : YcsbOperation::Insert;
      action.scan_rows = 1U + static_cast<std::size_t>(generator() % 100U);
    } else if (name == "ycsb_f") {
      action.operation = choice < 50U ? YcsbOperation::Read : YcsbOperation::ReadModifyWrite;
    } else {
      Fail("unknown YCSB workload");
    }

    if (action.operation == YcsbOperation::Insert) {
      action.key = scenario.rows + inserted++;
    } else if (name == "ycsb_d") {
      const auto current = scenario.rows + inserted;
      action.key = current - 1U - std::min(zipf(generator), current - 1U);
    } else {
      action.key = zipf(generator);
    }
    actions.push_back(action);
  }
  return actions;
}

auto RunYcsb(Backend &backend, const Scenario &scenario, const PortableData &data,
             const std::vector<YcsbAction> &actions) -> Observation {
  auto observation = Observation{};
  observation.call_latencies.reserve(std::min<std::size_t>(scenario.operations, 20'000));
  const auto started = Clock::now();
  for (const auto &action : actions) {
    if (action.operation == YcsbOperation::Read) {
      ObserveCall(observation, [&] {
        const auto value = backend.Get(data.keys[action.key]);
        if (!value) {
          Fail("YCSB read missed an existing key");
        }
        observation.digest ^= Mix64(value->size() + action.key);
      });
      observation.logical_read_bytes += scenario.key_bytes + scenario.value_bytes;
    } else if (action.operation == YcsbOperation::Update || action.operation == YcsbOperation::Insert) {
      const auto entry = Entry{data.keys[action.key], data.second_value};
      ObserveCall(observation, [&] { backend.Put(std::span{&entry, std::size_t{1}}); });
      observation.logical_write_bytes += scenario.key_bytes + scenario.value_bytes;
    } else if (action.operation == YcsbOperation::Scan) {
      ObserveCall(observation, [&] {
        const auto scan = backend.Scan(data.keys[action.key], action.scan_rows);
        observation.digest ^= scan.digest;
        observation.logical_read_bytes += scan.rows * (scenario.key_bytes + scenario.value_bytes);
      });
    } else {
      ObserveCall(observation, [&] {
        const auto value = backend.Get(data.keys[action.key]);
        if (!value) {
          Fail("YCSB read-modify-write missed an existing key");
        }
        const auto entry = Entry{data.keys[action.key], data.second_value};
        backend.Put(std::span{&entry, std::size_t{1}});
        observation.digest ^= Mix64(value->size() + action.key);
      });
      observation.logical_read_bytes += scenario.key_bytes + scenario.value_bytes;
      observation.logical_write_bytes += scenario.key_bytes + scenario.value_bytes;
    }
    ++observation.logical_operations;
  }
  observation.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return observation;
}

void AddPortableResults(const Scenario &scenario, Results &results, std::size_t trial, const Observation &observation,
                        const ProcessUsage &usage, const ProcessIo &io, const ProcessMemory &before_open,
                        const ProcessMemory &endpoint, const StorageUsage &storage) {
  const auto cpu_seconds = usage.user_seconds + usage.system_seconds;
  const auto throughput = static_cast<double>(observation.logical_operations) / observation.seconds;
  results.AddTrial(scenario, "throughput", "operations/second", trial, throughput);
  results.AddTrial(scenario, "call_latency_p50", "microseconds", trial, Percentile(observation.call_latencies, 0.50));
  results.AddTrial(scenario, "call_latency_p95", "microseconds", trial, Percentile(observation.call_latencies, 0.95));
  results.AddTrial(scenario, "call_latency_p99", "microseconds", trial, Percentile(observation.call_latencies, 0.99));
  results.AddTrial(scenario, "user_cpu_time", "seconds", trial, usage.user_seconds);
  results.AddTrial(scenario, "system_cpu_time", "seconds", trial, usage.system_seconds);
  results.AddTrial(scenario, "cpu_utilization", "cores", trial, cpu_seconds / observation.seconds);
  results.AddTrial(scenario, "minor_faults", "faults", trial, static_cast<double>(usage.minor_faults));
  results.AddTrial(scenario, "major_faults", "faults", trial, static_cast<double>(usage.major_faults));
  results.AddTrial(scenario, "voluntary_context_switches", "switches", trial,
                   static_cast<double>(usage.voluntary_context_switches));
  results.AddTrial(scenario, "involuntary_context_switches", "switches", trial,
                   static_cast<double>(usage.involuntary_context_switches));
  results.AddTrial(scenario, "read_syscalls", "calls", trial, static_cast<double>(io.read_syscalls));
  results.AddTrial(scenario, "write_syscalls", "calls", trial, static_cast<double>(io.write_syscalls));
  results.AddTrial(scenario, "storage_read_bytes", "bytes", trial, static_cast<double>(io.storage_read_bytes));
  results.AddTrial(scenario, "storage_write_bytes", "bytes", trial, static_cast<double>(io.storage_write_bytes));
  results.AddTrial(scenario, "storage_read_amplification", "bytes/logical_byte", trial,
                   observation.logical_read_bytes == 0 ? 0.0
                                                       : static_cast<double>(io.storage_read_bytes) /
                                                             static_cast<double>(observation.logical_read_bytes));
  results.AddTrial(scenario, "storage_write_amplification", "bytes/logical_byte", trial,
                   observation.logical_write_bytes == 0 ? 0.0
                                                        : static_cast<double>(io.storage_write_bytes) /
                                                              static_cast<double>(observation.logical_write_bytes));
  results.AddTrial(scenario, "process_rss_before_open", "bytes", trial,
                   static_cast<double>(before_open.resident_bytes));
  results.AddTrial(scenario, "process_rss_endpoint", "bytes", trial, static_cast<double>(endpoint.resident_bytes));
  results.AddTrial(scenario, "process_rss_growth", "bytes", trial,
                   static_cast<double>(endpoint.resident_bytes) - static_cast<double>(before_open.resident_bytes));
  results.AddTrial(scenario, "process_pss_before_open", "bytes", trial,
                   static_cast<double>(before_open.proportional_bytes));
  results.AddTrial(scenario, "process_pss_endpoint", "bytes", trial, static_cast<double>(endpoint.proportional_bytes));
  const auto engine_pss = endpoint.proportional_bytes >= before_open.proportional_bytes
                              ? endpoint.proportional_bytes - before_open.proportional_bytes
                              : 0;
  results.AddTrial(scenario, "engine_pss_bytes", "bytes", trial, static_cast<double>(engine_pss));
  results.AddTrial(scenario, "database_size", "bytes", trial, static_cast<double>(storage.bytes));
  results.AddTrial(scenario, "database_file_resident_bytes", "bytes", trial,
                   static_cast<double>(storage.residency.resident_bytes));
  results.AddTrial(scenario, "combined_observed_memory", "bytes", trial,
                   static_cast<double>(engine_pss + storage.residency.resident_bytes));
}

}  // namespace

void AddPortableScenarios(const Config &config, std::vector<Scenario> &scenarios) {
  const auto profile = SelectedProfile(config.profile);
  const auto add_db = [&](std::string name, std::string workload, std::size_t rows, std::size_t operations,
                          std::size_t batch = 1) {
    auto scenario = BasePortable("db." + name, "db_bench", std::move(workload), rows, operations, 100, profile.trials);
    scenario.batch = batch;
    scenario.cache_condition = IsFill(scenario.portable_workload) ? CacheCondition::Fresh : CacheCondition::Steady;
    scenarios.push_back(std::move(scenario));
  };
  add_db("fillseq", "fillseq", profile.write_records, profile.write_records);
  add_db("fillrandom", "fillrandom", profile.write_records, profile.write_records);
  add_db("fillbatch", "fillbatch", profile.write_records, profile.write_records, 16);
  add_db("overwrite", "overwrite", profile.write_records, profile.write_records);
  add_db("readrandom", "readrandom", profile.read_records, profile.read_operations);
  add_db("readseq", "readseq", profile.read_records, profile.read_operations);
  add_db("seekrandom", "seekrandom", profile.read_records, profile.read_operations);
  add_db("deleterandom", "deleterandom", profile.write_records, profile.write_records);

  for (const auto letter : {"a", "b", "c", "d", "e", "f"}) {
    auto scenario = BasePortable("ycsb." + std::string{letter}, "ycsb", "ycsb_" + std::string{letter},
                                 profile.ycsb_records, profile.ycsb_operations, 1'000, profile.trials);
    scenario.cache_condition = CacheCondition::Steady;
    scenario.default_enabled = false;
    scenarios.push_back(std::move(scenario));
  }
}

void AddTinyDbPortableScenarios(const Config &config, std::vector<Scenario> &scenarios) {
  const auto profile = SelectedProfile(config.profile);
  const auto rows_for = [](std::uint64_t bytes, std::size_t value_bytes) {
    return static_cast<std::size_t>(bytes / (16U + value_bytes + std::uint64_t{32}));
  };

  auto hot = BasePortable("cache.engine_hot", "cache", "readrandom", rows_for(kStandardPageCacheBytes / 2U, 128),
                          250'000, 128, profile.trials);
  hot.cache_condition = CacheCondition::EngineHot;
  scenarios.push_back(std::move(hot));

  auto pressure = BasePortable("cache.eviction_uniform", "cache", "readrandom",
                               rows_for(kStandardPageCacheBytes * 8U, 128), 250'000, 128, profile.trials);
  pressure.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(pressure));

  auto ranges = BasePortable("cache.range256", "cache", "readrange", rows_for(kStandardPageCacheBytes * 8U, 1U << 10U),
                             1'024, 1U << 10U, profile.trials);
  ranges.scan_rows = 256;
  ranges.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(ranges));

  auto large = BasePortable("values.write64k.batch4", "values", "filllarge", 96, 96, 64U << 10U, profile.trials);
  large.batch = 4;
  scenarios.push_back(std::move(large));
}

void BuildPortableFixture(const std::filesystem::path &root, const Scenario &scenario, const Config &config) {
  auto error = std::error_code{};
  if (std::filesystem::exists(root, error)) {
    Fail("portable fixture root already exists");
  }
  const auto data = MakeData(scenario);
  auto backend = OpenBackend(root, config, scenario);
  if (!IsFill(scenario.portable_workload)) {
    Populate(*backend, scenario, data);
    ValidateFixture(*backend, scenario, data);
    backend->StabilizeFixture();
  }
}

void RunPortableTrial(const std::filesystem::path &root, const Scenario &scenario, const Config &config,
                      Results &results) {
  const auto data = MakeData(scenario);
  const auto db_plan =
      scenario.family == "ycsb" ? std::vector<std::size_t>{} : MakeDbPlan(scenario, config.seed ^ 0x444242454E4348ULL);
  const auto ycsb_plan =
      scenario.family == "ycsb" ? MakeYcsbPlan(scenario, config.seed ^ 0x59435342ULL) : std::vector<YcsbAction>{};
  const auto memory_before_open = ObserveProcessMemory();
  auto backend = OpenBackend(root, config, scenario);
  PrimeReads(*backend, scenario);
  const auto io_before = ObserveProcessIo();
  const auto usage_before = ObserveProcessUsage();
  auto observation = scenario.family == "ycsb" ? RunYcsb(*backend, scenario, data, ycsb_plan)
                                               : RunDbWorkload(*backend, scenario, data, db_plan);
  backend->FinishReads();
  const auto usage = SubtractProcessUsage(ObserveProcessUsage(), usage_before);
  const auto io = SubtractProcessIo(ObserveProcessIo(), io_before);
  const auto memory_endpoint = ObserveProcessMemory();
  const auto storage = ObserveStorageUsage(root);

  if (IsFill(scenario.portable_workload) || scenario.portable_workload == "overwrite") {
    const auto expected = scenario.portable_workload == "overwrite" ? data.second_value : data.first_value;
    const auto first = backend->Get(data.keys[db_plan.front()]);
    const auto last = backend->Get(data.keys[db_plan.back()]);
    if (!first || !last || *first != expected || *last != expected) {
      Fail("portable write validation failed");
    }
  }

  AddPortableResults(scenario, results, config.trial_index, observation, usage, io, memory_before_open, memory_endpoint,
                     storage);
}

}  // namespace tinydb::bench
