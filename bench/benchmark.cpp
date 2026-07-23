#include "benchmark.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>

#ifndef TINYDB_BENCH_ENGINE_GIT_COMMIT
#define TINYDB_BENCH_ENGINE_GIT_COMMIT "unknown"
#endif

#ifndef TINYDB_BENCH_ENGINE_GIT_DIRTY
#define TINYDB_BENCH_ENGINE_GIT_DIRTY "null"
#endif

#ifndef TINYDB_BENCH_HARNESS_GIT_COMMIT
#define TINYDB_BENCH_HARNESS_GIT_COMMIT "unknown"
#endif

#ifndef TINYDB_BENCH_HARNESS_GIT_DIRTY
#define TINYDB_BENCH_HARNESS_GIT_DIRTY "null"
#endif

#ifndef TINYDB_BENCH_BUILD_TYPE
#define TINYDB_BENCH_BUILD_TYPE "unknown"
#endif

namespace tinydb::bench {
namespace {

constexpr auto kCpuTrials = std::size_t{5};
constexpr auto kIoTrials = std::size_t{3};
constexpr auto kLifecycleBytes = std::uint64_t{64U << 20U};
constexpr auto kIoBytes = std::uint64_t{64U << 20U};
constexpr auto kCommits = std::size_t{40};

auto WorkloadName(Workload workload) -> std::string_view {
  switch (workload) {
    case Workload::Portable:
      return "portable";
    case Workload::Concurrent:
      return "concurrent";
    case Workload::Checkpoint:
      return "checkpoint";
    case Workload::Recovery:
      return "recovery";
    case Workload::Churn:
      return "churn";
    case Workload::IoRead:
      return "io_read";
  }
  Fail("unknown workload kind");
}

auto AccessName(AccessPattern access) -> std::string_view {
  switch (access) {
    case AccessPattern::Sequential:
      return "sequential";
    case AccessPattern::Uniform:
      return "uniform";
  }
  Fail("unknown access pattern");
}

auto CacheConditionName(CacheCondition condition) -> std::string_view {
  switch (condition) {
    case CacheCondition::Fresh:
      return "fresh";
    case CacheCondition::EngineHot:
      return "engine_hot";
    case CacheCondition::Steady:
      return "steady";
    case CacheCondition::FileCold:
      return "file_cold";
    case CacheCondition::OsWarm:
      return "os_warm";
  }
  Fail("unknown cache condition");
}

auto FixturePolicyName(FixturePolicy policy) -> std::string_view {
  return policy == FixturePolicy::Shared ? "shared" : "native";
}

auto DirectionName(MetricDirection direction) -> std::string_view {
  return direction == MetricDirection::Higher ? "higher" : "lower";
}

auto ScopeName(SampleScope scope) -> std::string_view { return scope == SampleScope::Trial ? "trial" : "observation"; }

auto RowsForBytes(std::uint64_t bytes, std::size_t key_bytes, std::size_t value_bytes) -> std::size_t {
  constexpr auto estimated_page_overhead = std::uint64_t{32};
  const auto row_bytes =
      static_cast<std::uint64_t>(key_bytes) + static_cast<std::uint64_t>(value_bytes) + estimated_page_overhead;
  return static_cast<std::size_t>(std::max<std::uint64_t>(1, bytes / row_bytes));
}

auto RowsForRatio(std::size_t page_cache_bytes, std::size_t numerator, std::size_t denominator, std::size_t key_bytes,
                  std::size_t value_bytes) -> std::size_t {
  return RowsForBytes(static_cast<std::uint64_t>(page_cache_bytes) * numerator / denominator, key_bytes, value_bytes);
}

auto BaseScenario(std::string name, std::string family, Workload workload, std::string primary_metric,
                  MetricDirection direction, double meaningful_difference) -> Scenario {
  auto scenario = Scenario{};
  scenario.name = std::move(name);
  scenario.family = std::move(family);
  scenario.workload = workload;
  scenario.primary_metric = std::move(primary_metric);
  scenario.primary_direction = direction;
  scenario.meaningful_difference = meaningful_difference;
  scenario.trials = kCpuTrials;
  return scenario;
}

void AddConcurrentScenario(std::vector<Scenario> &scenarios) {
  auto scenario = BaseScenario("concurrent.writer.readers4", "concurrency", Workload::Concurrent, "reader_throughput",
                               MetricDirection::Higher, 0.08);
  scenario.rows = RowsForRatio(scenario.page_cache_bytes, 2, 1, scenario.key_bytes, scenario.value_bytes);
  scenario.commits = kCommits;
  scenario.batch = 16;
  scenario.reader_threads = 4;
  scenario.preparation_rounds = 1;
  scenario.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(scenario));
}

void AddLifecycleScenarios(std::vector<Scenario> &scenarios) {
  const auto rows = RowsForBytes(kLifecycleBytes, 16, 1U << 10U);

  auto checkpoint =
      BaseScenario("checkpoint.64MiB", "checkpoint", Workload::Checkpoint, "latency", MetricDirection::Lower, 0.08);
  checkpoint.rows = rows;
  checkpoint.value_bytes = 1U << 10U;
  checkpoint.target_bytes = kLifecycleBytes;
  checkpoint.trials = kIoTrials;
  scenarios.push_back(std::move(checkpoint));

  auto recovery = BaseScenario("recovery.os_warm.64MiB", "recovery", Workload::Recovery, "open_latency",
                               MetricDirection::Lower, 0.08);
  recovery.rows = rows;
  recovery.value_bytes = 1U << 10U;
  recovery.target_bytes = kLifecycleBytes;
  recovery.trials = kIoTrials;
  recovery.cache_condition = CacheCondition::OsWarm;
  scenarios.push_back(std::move(recovery));
}

void AddChurnScenario(std::vector<Scenario> &scenarios) {
  auto scenario =
      BaseScenario("churn.steady_state", "churn", Workload::Churn, "throughput", MetricDirection::Higher, 0.10);
  scenario.value_bytes = 8U << 10U;
  scenario.rows = RowsForRatio(scenario.page_cache_bytes, 2, 1, scenario.key_bytes, scenario.value_bytes);
  scenario.trials = kIoTrials;
  scenario.churn_warmup_rounds = 2;
  scenario.churn_measured_rounds = 5;
  scenario.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(scenario));
}

void AddColdIoScenarios(std::vector<Scenario> &scenarios) {
  const auto add = [&](std::string name, AccessPattern access, std::size_t value_bytes, std::size_t operations,
                       FixturePolicy fixture_policy = FixturePolicy::Shared) {
    auto scenario =
        BaseScenario(std::move(name), "cold_io", Workload::IoRead, "throughput", MetricDirection::Higher, 0.05);
    scenario.access = access;
    scenario.value_bytes = value_bytes;
    scenario.rows = RowsForBytes(kIoBytes, scenario.key_bytes, value_bytes);
    scenario.target_bytes = kIoBytes;
    scenario.operations = operations;
    scenario.trials = kIoTrials;
    scenario.cache_condition = CacheCondition::FileCold;
    scenario.fixture_policy = fixture_policy;
    scenarios.push_back(std::move(scenario));
  };

  add("read.cold.scan.64MiB", AccessPattern::Sequential, 1U << 10U, 0);
  add("read.cold.random.64MiB", AccessPattern::Uniform, 1U << 10U, 2'048);
  add("read.cold.large-values.compat.64MiB", AccessPattern::Sequential, 64U << 10U, 0);
  add("read.cold.large-values.native.64MiB", AccessPattern::Sequential, 64U << 10U, 0, FixturePolicy::Native);
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

void Usage() {
  std::puts(
      "usage: TinyDB_bench --describe\n"
      "       TinyDB_bench --list [--family NAME] [--filter TEXT] [--profile NAME]\n"
      "       TinyDB_bench --scenario NAME --build-fixture ROOT\n"
      "       TinyDB_bench --scenario NAME --run-trial ROOT --dataset-id SHA256\n"
      "                    --trial-index N --seed N\n"
      "                    [--page-cache-bytes BYTES] [--semantics durable|native]\n");
}

auto JsonEscape(std::string_view value) -> std::string {
  auto escaped = std::string{};
  escaped.reserve(value.size());
  for (const auto byte : value) {
    switch (byte) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += static_cast<unsigned char>(byte) < 0x20U ? '?' : byte;
    }
  }
  return escaped;
}

auto Compiler() -> std::string {
#if defined(__clang__)
  return "Clang " __clang_version__;
#elif defined(__GNUC__)
  return "GCC " __VERSION__;
#elif defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

}  // namespace

[[noreturn]] void Fail(std::string_view message) {
  std::fprintf(stderr, "benchmark failed: %.*s\n", static_cast<int>(message.size()), message.data());
  std::exit(1);
}

auto ParseConfig(int argc, char **argv) -> Config {
  auto config = Config{};

  auto selected_mode = false;
  for (auto index = 1; index < argc; ++index) {
    const auto flag = std::string_view{argv[index]};
    if (flag == "--help") {
      Usage();
      std::exit(0);
    }
    if (flag == "--list" || flag == "--describe") {
      if (selected_mode) {
        Fail("benchmark modes are mutually exclusive");
      }
      config.mode = flag == "--list" ? BenchmarkMode::List : BenchmarkMode::Describe;
      selected_mode = true;
      continue;
    }
    if (index + 1 >= argc) {
      Fail("missing benchmark option value");
    }
    const auto value = std::string_view{argv[++index]};
    if (flag == "--family") {
      config.families.emplace_back(value);
    } else if (flag == "--filter") {
      config.filter = value;
    } else if (flag == "--scenario") {
      config.scenario = value;
    } else if (flag == "--build-fixture" || flag == "--run-trial") {
      if (selected_mode) {
        Fail("benchmark modes are mutually exclusive");
      }
      config.mode = flag == "--build-fixture" ? BenchmarkMode::BuildFixture : BenchmarkMode::RunTrial;
      config.fixture = value;
      selected_mode = true;
    } else if (flag == "--dataset-id") {
      config.dataset_id = value;
    } else if (flag == "--trial-index") {
      config.trial_index = AsSize(ParseUnsigned(value, flag), flag);
    } else if (flag == "--page-cache-bytes") {
      config.page_cache_bytes = AsSize(ParseUnsigned(value, flag), flag);
      if (*config.page_cache_bytes == 0) {
        Fail("--page-cache-bytes must be greater than zero");
      }
    } else if (flag == "--seed") {
      config.seed = ParseUnsigned(value, flag);
    } else if (flag == "--profile") {
      config.profile = value;
      if (config.profile != "smoke" && config.profile != "standard" && config.profile != "soak") {
        Fail("--profile must be smoke, standard, or soak");
      }
    } else if (flag == "--semantics") {
      config.semantics = value;
      if (config.semantics != "durable" && config.semantics != "native") {
        Fail("--semantics must be durable or native");
      }
    } else {
      Fail(std::string("unknown benchmark option: ") + std::string(flag));
    }
  }

  if (config.mode != BenchmarkMode::List && config.mode != BenchmarkMode::Describe &&
      (!config.scenario || config.fixture.empty())) {
    Fail("fixture modes require --scenario and a database path");
  }
  if (config.mode == BenchmarkMode::RunTrial && config.dataset_id.empty()) {
    Fail("--run-trial requires --dataset-id");
  }
  if (config.page_cache_bytes && config.mode != BenchmarkMode::RunTrial) {
    Fail("--page-cache-bytes requires --run-trial");
  }
  return config;
}

auto BuildScenarios(const Config &config) -> std::vector<Scenario> {
  auto scenarios = std::vector<Scenario>{};
  AddPortableScenarios(config, scenarios);
#if defined(KVBENCH_TINYDB)
  AddTinyDbPortableScenarios(config, scenarios);
  AddConcurrentScenario(scenarios);
  AddLifecycleScenarios(scenarios);
  AddChurnScenario(scenarios);
  AddColdIoScenarios(scenarios);
#endif

  std::erase_if(scenarios, [&](const Scenario &scenario) {
    const auto family_selected = config.scenario ? true
                                 : config.families.empty()
                                     ? scenario.default_enabled
                                     : std::ranges::find(config.families, scenario.family) != config.families.end();
    return !family_selected || (config.filter && !scenario.name.contains(*config.filter)) ||
           (config.scenario && scenario.name != *config.scenario);
  });
  if (scenarios.empty()) {
    Fail("no scenarios match the requested selection");
  }
  return scenarios;
}

void PrintIdentity() {
  const auto identity = Identity();
  std::printf(
      "{\"backend\":\"%s\",\"format_family\":\"%s\","
      "\"tinydb_qualification\":%s,\"always_durable\":%s,"
      "\"engine_revision\":\"%s\",\"engine_dirty\":%s,"
      "\"harness_revision\":\"%s\",\"harness_dirty\":%s,"
      "\"build_type\":\"%s\",\"compiler\":\"%s\"}\n",
      identity.name.c_str(), identity.format_family.c_str(), identity.tinydb_qualification ? "true" : "false",
      identity.always_durable ? "true" : "false", TINYDB_BENCH_ENGINE_GIT_COMMIT, TINYDB_BENCH_ENGINE_GIT_DIRTY,
      TINYDB_BENCH_HARNESS_GIT_COMMIT, TINYDB_BENCH_HARNESS_GIT_DIRTY, TINYDB_BENCH_BUILD_TYPE,
      JsonEscape(Compiler()).c_str());
}

void PrintScenarios(const std::vector<Scenario> &scenarios) {
  std::puts(
      "scenario,family,workload,portable_workload,access,cache_condition,fixture_policy,primary_metric,primary_"
      "direction,"
      "meaningful_difference,"
      "rows,key_bytes,value_bytes,page_cache_bytes,trials,preparation_rounds,commits,batch,"
      "scan_rows,operations,reader_threads,churn_warmup_rounds,churn_measured_rounds,target_bytes");
  for (const auto &scenario : scenarios) {
    std::printf(
        "%s,%s,%.*s,%s,%.*s,%.*s,%.*s,%s,%.*s,%.6f,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
        "%llu\n",
        scenario.name.c_str(), scenario.family.c_str(), static_cast<int>(WorkloadName(scenario.workload).size()),
        WorkloadName(scenario.workload).data(), scenario.portable_workload.c_str(),
        static_cast<int>(AccessName(scenario.access).size()), AccessName(scenario.access).data(),
        static_cast<int>(CacheConditionName(scenario.cache_condition).size()),
        CacheConditionName(scenario.cache_condition).data(),
        static_cast<int>(FixturePolicyName(scenario.fixture_policy).size()),
        FixturePolicyName(scenario.fixture_policy).data(), scenario.primary_metric.c_str(),
        static_cast<int>(DirectionName(scenario.primary_direction).size()),
        DirectionName(scenario.primary_direction).data(), scenario.meaningful_difference, scenario.rows,
        scenario.key_bytes, scenario.value_bytes, scenario.page_cache_bytes, scenario.trials,
        scenario.preparation_rounds, scenario.commits, scenario.batch, scenario.scan_rows, scenario.operations,
        scenario.reader_threads, scenario.churn_warmup_rounds, scenario.churn_measured_rounds,
        static_cast<unsigned long long>(scenario.target_bytes));
  }
}

Results::Results(std::uint64_t trial_seed, std::string dataset_id)
    : trial_seed_(trial_seed), dataset_id_(std::move(dataset_id)) {}

void Results::Add(const Scenario &scenario, std::string_view metric, std::string_view unit, SampleScope scope,
                  std::size_t trial, std::size_t observation, double value) {
  samples_.push_back(Sample{scenario.name, scenario.family, trial_seed_, dataset_id_, std::string(metric),
                            std::string(unit), scope, trial, observation, value});
}

void Results::AddTrial(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
                       double value) {
  Add(scenario, metric, unit, SampleScope::Trial, trial, 0, value);
}

void Results::AddObservation(const Scenario &scenario, std::string_view metric, std::string_view unit,
                             std::size_t trial, std::size_t observation, double value) {
  Add(scenario, metric, unit, SampleScope::Observation, trial, observation, value);
}

void Results::Print() const {
  std::cout << "scenario,family,trial_seed,dataset_id,metric,unit,scope,trial,observation,value\n"
            << std::setprecision(17);
  for (const auto &sample : samples_) {
    std::cout << sample.scenario << ',' << sample.family << ',' << sample.trial_seed << ',' << sample.dataset_id << ','
              << sample.metric << ',' << sample.unit << ',' << ScopeName(sample.scope) << ',' << sample.trial << ','
              << sample.observation << ',' << sample.value << '\n';
  }
  std::cout.flush();
  if (!std::cout) {
    Fail("cannot write benchmark samples");
  }
}

auto MakeKey(std::size_t row, std::size_t bytes) -> std::string {
  constexpr auto ordered_prefix_bytes = std::size_t{16};
  if (bytes < ordered_prefix_bytes) {
    Fail("benchmark keys must be at least 16 bytes");
  }
  auto digits = std::array<char, 32>{};
  const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), row);
  if (result.ec != std::errc{}) {
    Fail("cannot encode benchmark key");
  }
  const auto digit_count = static_cast<std::size_t>(result.ptr - digits.data());
  if (digit_count > ordered_prefix_bytes) {
    Fail("benchmark row number exceeds key encoding");
  }
  auto key = std::string(ordered_prefix_bytes - digit_count, '0');
  key.insert(key.end(), digits.begin(), digits.begin() + static_cast<std::ptrdiff_t>(digit_count));
  while (key.size() < bytes) {
    key.push_back(static_cast<char>('a' + (row + key.size()) % 26U));
  }
  return key;
}

auto MakeValue(std::size_t row, std::size_t bytes, std::size_t generation) -> std::string {
  auto value = std::string(bytes, static_cast<char>('a' + (row + generation) % 26U));
  const auto prefix = std::to_string(row) + ':' + std::to_string(generation) + ':';
  for (std::size_t index = 0; index < std::min(prefix.size(), value.size()); ++index) {
    value[index] = prefix[index];
  }
  return value;
}

auto MakeDataset(std::size_t rows, std::size_t key_bytes, std::size_t value_bytes) -> Dataset {
  auto data = Dataset{};
  data.keys.reserve(rows);
  data.first_values.reserve(rows);
  data.second_values.reserve(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    data.keys.push_back(MakeKey(row, key_bytes));
    data.first_values.push_back(MakeValue(row, value_bytes, 0));
    data.second_values.push_back(MakeValue(row, value_bytes, 1));
    data.logical_bytes += data.keys.back().size() + data.first_values.back().size();
  }
  return data;
}

void BuildScenarioFixture(const std::filesystem::path &path, const Scenario &scenario, const Config &config) {
  if (scenario.workload == Workload::Portable) {
    BuildPortableFixture(path, scenario, config);
    return;
  }
#if defined(KVBENCH_TINYDB)
  BuildTinyDbFixture(path, scenario);
#else
  Fail("selected backend does not support TinyDB qualification workloads");
#endif
}

void RunTrial(const std::filesystem::path &path, const Scenario &scenario, const Config &config, Results &results) {
  if (scenario.workload == Workload::Portable) {
    RunPortableTrial(path, scenario, config, results);
    return;
  }
#if defined(KVBENCH_TINYDB)
  RunTinyDbTrial(path, scenario, config, results);
#else
  Fail("selected backend does not support TinyDB qualification workloads");
#endif
}

}  // namespace tinydb::bench

auto main(int argc, char **argv) -> int {
  using namespace tinydb::bench;

  const auto config = ParseConfig(argc, argv);
  if (config.mode == BenchmarkMode::Describe) {
    PrintIdentity();
    return 0;
  }
  const auto scenarios = BuildScenarios(config);
  if (config.mode == BenchmarkMode::List) {
    PrintScenarios(scenarios);
    return 0;
  }
  if (scenarios.size() != 1U) {
    Fail("fixture operations require exactly one scenario");
  }
  if (config.mode == BenchmarkMode::BuildFixture) {
    BuildScenarioFixture(config.fixture, scenarios.front(), config);
    return 0;
  }

  auto results = Results{config.seed, config.dataset_id};
  RunTrial(config.fixture, scenarios.front(), config, results);
  results.Print();
  return 0;
}
