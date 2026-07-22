#include "benchmark.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <system_error>
#include <thread>

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

constexpr auto kCacheBytes = std::size_t{8U << 20U};
constexpr auto kCpuTrials = std::size_t{5};
constexpr auto kIoTrials = std::size_t{3};
constexpr auto kLifecycleBytes = std::uint64_t{64U << 20U};
constexpr auto kIoBytes = std::uint64_t{64U << 20U};
constexpr auto kMeasurement = std::chrono::milliseconds{750};
constexpr auto kWarmup = std::chrono::milliseconds{250};
constexpr auto kCommits = std::size_t{40};

auto CheckedMultiply(std::size_t left, std::size_t right, std::string_view description) -> std::size_t {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    Fail(std::string(description) + " is too large");
  }
  return left * right;
}

auto WorkloadName(Workload workload) -> std::string_view {
  switch (workload) {
    case Workload::Put:
      return "put";
    case Workload::PointRead:
      return "point_read";
    case Workload::Scan:
      return "scan";
    case Workload::Mixed:
      return "mixed";
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
    case AccessPattern::Hotspot:
      return "hotspot";
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

auto DirectionName(MetricDirection direction) -> std::string_view {
  return direction == MetricDirection::Higher ? "higher" : "lower";
}

auto ScopeName(SampleScope scope) -> std::string_view {
  return scope == SampleScope::Trial ? "trial" : "observation";
}

auto RowsForBytes(std::uint64_t bytes, std::size_t key_bytes, std::size_t value_bytes) -> std::size_t {
  constexpr auto estimated_page_overhead = std::uint64_t{32};
  const auto row_bytes =
      static_cast<std::uint64_t>(key_bytes) + static_cast<std::uint64_t>(value_bytes) + estimated_page_overhead;
  return static_cast<std::size_t>(std::max<std::uint64_t>(1, bytes / row_bytes));
}

auto RowsForRatio(std::size_t numerator, std::size_t denominator, std::size_t key_bytes,
                  std::size_t value_bytes) -> std::size_t {
  return RowsForBytes(static_cast<std::uint64_t>(kCacheBytes) * numerator / denominator, key_bytes, value_bytes);
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
  scenario.cache_bytes = kCacheBytes;
  scenario.trials = kCpuTrials;
  return scenario;
}

void AddWriteScenarios(std::vector<Scenario> &scenarios) {
  const auto add = [&](std::string name, std::size_t value_bytes, std::size_t batch, bool overwrite, bool random,
                       std::size_t commits = kCommits) {
    auto scenario = BaseScenario(std::move(name), "writes", Workload::Put, "throughput", MetricDirection::Higher,
                                 0.05);
    scenario.value_bytes = value_bytes;
    scenario.commits = commits;
    scenario.batch = batch;
    scenario.rows = CheckedMultiply(commits, batch, "write rows");
    scenario.overwrite = overwrite;
    scenario.random_write_order = random;
    scenario.access = random ? AccessPattern::Uniform : AccessPattern::Sequential;
    scenarios.push_back(std::move(scenario));
  };

  add("put.insert.sequential.batch1", 128, 1, false, false);
  add("put.insert.random.batch16", 128, 16, false, true);
  add("put.overwrite.random.batch16", 128, 16, true, true);
  add("put.value64k.batch4", 64U << 10U, 4, false, true, 24);
}

void AddReadScenarios(std::vector<Scenario> &scenarios) {
  const auto add = [&](std::string name, std::size_t numerator, std::size_t denominator,
                       CacheCondition cache_condition) {
    auto scenario = BaseScenario(std::move(name), "reads", Workload::PointRead, "throughput",
                                 MetricDirection::Higher, 0.03);
    scenario.rows = RowsForRatio(numerator, denominator, scenario.key_bytes, scenario.value_bytes);
    scenario.cache_ratio_numerator = numerator;
    scenario.cache_ratio_denominator = denominator;
    scenario.cache_condition = cache_condition;
    scenario.warmup = kWarmup;
    scenario.measurement = kMeasurement;
    scenarios.push_back(std::move(scenario));
  };

  add("read.engine_hot", 1, 2, CacheCondition::EngineHot);
  add("read.eviction.uniform", 8, 1, CacheCondition::Steady);
}

void AddScanScenario(std::vector<Scenario> &scenarios) {
  auto scenario = BaseScenario("scan.range256.values", "scans", Workload::Scan, "throughput",
                               MetricDirection::Higher, 0.03);
  scenario.rows = RowsForRatio(8, 1, scenario.key_bytes, 1U << 10U);
  scenario.value_bytes = 1U << 10U;
  scenario.cache_ratio_numerator = 8;
  scenario.scan_rows = 256;
  scenario.cache_condition = CacheCondition::Steady;
  scenario.warmup = kWarmup;
  scenario.measurement = kMeasurement;
  scenarios.push_back(std::move(scenario));
}

void AddMixedScenario(std::vector<Scenario> &scenarios) {
  auto scenario = BaseScenario("mixed.80r12u4i4d.uniform", "mixed", Workload::Mixed, "throughput",
                               MetricDirection::Higher, 0.05);
  scenario.rows = RowsForRatio(8, 1, scenario.key_bytes, scenario.value_bytes);
  scenario.cache_ratio_numerator = 8;
  scenario.commits = kCommits;
  scenario.batch = 25;
  scenario.preparation_rounds = 1;
  scenario.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(scenario));
}

void AddConcurrentScenario(std::vector<Scenario> &scenarios) {
  auto scenario = BaseScenario("concurrent.writer.readers4", "concurrency", Workload::Concurrent,
                               "reader_throughput", MetricDirection::Higher, 0.08);
  scenario.rows = RowsForRatio(2, 1, scenario.key_bytes, scenario.value_bytes);
  scenario.cache_ratio_numerator = 2;
  scenario.commits = kCommits;
  scenario.batch = 16;
  scenario.reader_threads = 4;
  scenario.preparation_rounds = 1;
  scenario.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(scenario));
}

void AddLifecycleScenarios(std::vector<Scenario> &scenarios) {
  const auto rows = RowsForBytes(kLifecycleBytes, 16, 1U << 10U);

  auto checkpoint = BaseScenario("checkpoint.64MiB", "checkpoint", Workload::Checkpoint, "latency",
                                 MetricDirection::Lower, 0.08);
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
  auto scenario = BaseScenario("churn.steady_state", "churn", Workload::Churn, "throughput",
                               MetricDirection::Higher, 0.10);
  scenario.rows = RowsForRatio(2, 1, scenario.key_bytes, scenario.value_bytes);
  scenario.cache_ratio_numerator = 2;
  scenario.trials = kIoTrials;
  scenario.churn_warmup_rounds = 2;
  scenario.churn_measured_rounds = 5;
  scenario.cache_condition = CacheCondition::Steady;
  scenarios.push_back(std::move(scenario));
}

void AddColdIoScenarios(std::vector<Scenario> &scenarios) {
  const auto add = [&](std::string name, AccessPattern access, std::size_t value_bytes, std::size_t operations) {
    auto scenario = BaseScenario(std::move(name), "cold_io", Workload::IoRead, "throughput",
                                 MetricDirection::Higher, 0.05);
    scenario.access = access;
    scenario.value_bytes = value_bytes;
    scenario.rows = RowsForBytes(kIoBytes, scenario.key_bytes, value_bytes);
    scenario.target_bytes = kIoBytes;
    scenario.operations = operations;
    scenario.trials = kIoTrials;
    scenario.cache_condition = CacheCondition::FileCold;
    scenario.drop_file_cache = true;
    scenarios.push_back(std::move(scenario));
  };

  add("read.cold.scan.64MiB", AccessPattern::Sequential, 1U << 10U, 0);
  add("read.cold.random.64MiB", AccessPattern::Uniform, 1U << 10U, 2'048);
  add("read.cold.large-values.64MiB", AccessPattern::Sequential, 64U << 10U, 0);
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

auto Timestamp(std::chrono::system_clock::time_point time) -> std::string {
  const auto raw = std::chrono::system_clock::to_time_t(time);
  auto utc = std::tm{};
  (void)::gmtime_r(&raw, &utc);
  auto buffer = std::array<char, 32>{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer.data();
}

void Usage() {
  std::puts(
      "usage: TinyDB_bench --list [--family NAME] [--filter TEXT]\n"
      "       TinyDB_bench --scenario NAME --build-fixture DATABASE\n"
      "       TinyDB_bench --scenario NAME --run-trial DATABASE --fixture-id SHA256\n"
      "                    --trial-index N --seed N --output PATH\n");
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

auto ReadFirstValue(const std::filesystem::path &path, std::string_view prefix) -> std::string {
  auto input = std::ifstream{path};
  auto line = std::string{};
  while (std::getline(input, line)) {
    if (!line.starts_with(prefix)) {
      continue;
    }
    const auto delimiter = line.find(':');
    if (delimiter == std::string::npos) {
      continue;
    }
    auto value = line.substr(delimiter + 1U);
    value.erase(0, value.find_first_not_of(" \t"));
    return value;
  }
  return "unknown";
}

auto ReadFirstLine(const std::filesystem::path &path) -> std::string {
  auto input = std::ifstream{path};
  auto line = std::string{};
  if (!std::getline(input, line)) {
    return "unknown";
  }
  const auto end = line.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) {
    line.clear();
  } else {
    line.erase(end + 1U);
  }
  return line;
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

void Check(const Status &status, std::string_view operation) {
  if (!status.Ok()) {
    Fail(std::string(operation) + ": " + status.ToString());
  }
}

auto ParseConfig(int argc, char **argv) -> Config {
  auto config = Config{};
  config.arguments.reserve(static_cast<std::size_t>(argc));
  for (auto index = 0; index < argc; ++index) {
    config.arguments.emplace_back(argv[index]);
  }

  auto selected_mode = false;
  for (auto index = 1; index < argc; ++index) {
    const auto flag = std::string_view{argv[index]};
    if (flag == "--help") {
      Usage();
      std::exit(0);
    }
    if (flag == "--list") {
      if (selected_mode) {
        Fail("benchmark modes are mutually exclusive");
      }
      config.mode = BenchmarkMode::List;
      selected_mode = true;
      continue;
    }
    if (index + 1 >= argc) {
      Fail("missing benchmark option value");
    }
    const auto value = std::string_view{argv[++index]};
    if (flag == "--output") {
      config.output = value;
    } else if (flag == "--family") {
      config.family = value;
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
    } else if (flag == "--fixture-id") {
      config.fixture_id = value;
    } else if (flag == "--trial-index") {
      config.trial_index = AsSize(ParseUnsigned(value, flag), flag);
    } else if (flag == "--seed") {
      config.seed = ParseUnsigned(value, flag);
    } else {
      Fail(std::string("unknown benchmark option: ") + std::string(flag));
    }
  }

  if (config.mode != BenchmarkMode::List && (!config.scenario || config.fixture.empty())) {
    Fail("fixture modes require --scenario and a database path");
  }
  if (config.mode == BenchmarkMode::RunTrial && (config.fixture_id.empty() || config.output.empty())) {
    Fail("--run-trial requires --fixture-id and --output");
  }
  return config;
}

auto BuildScenarios(const Config &config) -> std::vector<Scenario> {
  auto scenarios = std::vector<Scenario>{};
  AddWriteScenarios(scenarios);
  AddReadScenarios(scenarios);
  AddScanScenario(scenarios);
  AddMixedScenario(scenarios);
  AddConcurrentScenario(scenarios);
  AddLifecycleScenarios(scenarios);
  AddChurnScenario(scenarios);
  AddColdIoScenarios(scenarios);

  std::erase_if(scenarios, [&](const Scenario &scenario) {
    return (config.family && scenario.family != *config.family) ||
           (config.filter && !scenario.name.contains(*config.filter)) ||
           (config.scenario && scenario.name != *config.scenario);
  });
  if (scenarios.empty()) {
    Fail("no scenarios match the requested selection");
  }
  return scenarios;
}

void PrintScenarios(const std::vector<Scenario> &scenarios) {
  std::puts(
      "scenario,family,workload,access,cache_condition,primary_metric,primary_direction,meaningful_difference,"
      "rows,key_bytes,value_bytes,cache_bytes,trials,warmup_ms,measurement_ms,preparation_rounds,commits,batch,"
      "scan_rows,operations,reader_threads,churn_warmup_rounds,churn_measured_rounds,target_bytes,overwrite,"
      "random_write_order,transaction_scoped_reads,copy_values,drop_file_cache");
  for (const auto &scenario : scenarios) {
    std::printf(
        "%s,%s,%.*s,%.*s,%.*s,%s,%.*s,%.6f,%zu,%zu,%zu,%zu,%zu,%lld,%lld,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
        "%llu,%s,%s,%s,%s,%s\n",
        scenario.name.c_str(), scenario.family.c_str(), static_cast<int>(WorkloadName(scenario.workload).size()),
        WorkloadName(scenario.workload).data(), static_cast<int>(AccessName(scenario.access).size()),
        AccessName(scenario.access).data(), static_cast<int>(CacheConditionName(scenario.cache_condition).size()),
        CacheConditionName(scenario.cache_condition).data(), scenario.primary_metric.c_str(),
        static_cast<int>(DirectionName(scenario.primary_direction).size()),
        DirectionName(scenario.primary_direction).data(), scenario.meaningful_difference, scenario.rows,
        scenario.key_bytes, scenario.value_bytes, scenario.cache_bytes, scenario.trials,
        static_cast<long long>(scenario.warmup.count()), static_cast<long long>(scenario.measurement.count()),
        scenario.preparation_rounds, scenario.commits, scenario.batch, scenario.scan_rows, scenario.operations,
        scenario.reader_threads, scenario.churn_warmup_rounds, scenario.churn_measured_rounds,
        static_cast<unsigned long long>(scenario.target_bytes), scenario.overwrite ? "true" : "false",
        scenario.random_write_order ? "true" : "false", scenario.transaction_scoped_reads ? "true" : "false",
        scenario.copy_values ? "true" : "false", scenario.drop_file_cache ? "true" : "false");
  }
}

Results::Results(std::uint64_t trial_seed, std::string fixture_id)
    : trial_seed_(trial_seed), fixture_id_(std::move(fixture_id)) {}

void Results::Add(const Scenario &scenario, std::string_view metric, std::string_view unit, SampleScope scope,
                  std::size_t trial, std::size_t observation, double value) {
  samples_.push_back(Sample{scenario.name, scenario.family, trial_seed_, fixture_id_, std::string(metric),
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

void Results::Write(const std::filesystem::path &directory) const {
  auto samples = std::ofstream{directory / "samples.csv"};
  if (!samples) {
    Fail("cannot open samples.csv");
  }
  samples << "scenario,family,trial_seed,fixture_id,metric,unit,scope,trial,observation,value\n";
  samples << std::setprecision(17);
  for (const auto &sample : samples_) {
    samples << sample.scenario << ',' << sample.family << ',' << sample.trial_seed << ',' << sample.fixture_id << ','
            << sample.metric << ',' << sample.unit << ',' << ScopeName(sample.scope) << ',' << sample.trial << ','
            << sample.observation << ',' << sample.value << '\n';
  }
  samples.flush();
  if (!samples) {
    Fail("cannot finish samples.csv");
  }
}

void WriteMetadata(const Config &config, const Scenario &scenario, const std::filesystem::path &directory,
                   std::chrono::system_clock::time_point started, std::chrono::steady_clock::duration elapsed) {
  auto system = utsname{};
  const auto uname_ok = ::uname(&system) == 0;
  const auto fixture_directory = config.fixture.parent_path();
  struct statvfs filesystem {};
  const auto statvfs_ok = ::statvfs(fixture_directory.c_str(), &filesystem) == 0;
  struct statfs filesystem_type {};
  const auto statfs_ok = ::statfs(fixture_directory.c_str(), &filesystem_type) == 0;

  auto output = std::ofstream{directory / "metadata.json"};
  if (!output) {
    Fail("cannot open metadata.json");
  }
  output << "{\n"
         << "  \"suite_version\": 6,\n"
         << "  \"trial_seed\": " << config.seed << ",\n"
         << "  \"trial\": " << config.trial_index << ",\n"
         << "  \"fixture_id\": \"" << JsonEscape(config.fixture_id) << "\",\n"
         << "  \"started_utc\": \"" << Timestamp(started) << "\",\n"
         << "  \"elapsed_seconds\": " << std::chrono::duration<double>(elapsed).count() << ",\n"
         << "  \"engine_git_commit\": \"" << TINYDB_BENCH_ENGINE_GIT_COMMIT << "\",\n"
         << "  \"engine_git_dirty\": " << TINYDB_BENCH_ENGINE_GIT_DIRTY << ",\n"
         << "  \"harness_git_commit\": \"" << TINYDB_BENCH_HARNESS_GIT_COMMIT << "\",\n"
         << "  \"harness_git_dirty\": " << TINYDB_BENCH_HARNESS_GIT_DIRTY << ",\n"
         << "  \"build_type\": \"" << JsonEscape(TINYDB_BENCH_BUILD_TYPE) << "\",\n"
         << "  \"compiler\": \"" << JsonEscape(Compiler()) << "\",\n"
         << "  \"system\": {\n"
         << "    \"hostname\": \"" << JsonEscape(uname_ok ? system.nodename : "unknown") << "\",\n"
         << "    \"kernel\": \"" << JsonEscape(uname_ok ? system.release : "unknown") << "\",\n"
         << "    \"architecture\": \"" << JsonEscape(uname_ok ? system.machine : "unknown") << "\",\n"
         << "    \"cpu\": \"" << JsonEscape(ReadFirstValue("/proc/cpuinfo", "model name")) << "\",\n"
         << "    \"cpu_governor\": \""
         << JsonEscape(ReadFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")) << "\",\n"
         << "    \"memory\": \"" << JsonEscape(ReadFirstValue("/proc/meminfo", "MemTotal")) << "\",\n"
         << "    \"hardware_threads\": " << std::thread::hardware_concurrency() << "\n"
         << "  },\n"
         << "  \"fixture_storage\": {\n"
         << "    \"directory\": \"" << JsonEscape(fixture_directory.string()) << "\",\n"
         << "    \"filesystem_magic\": " << (statfs_ok ? filesystem_type.f_type : 0) << ",\n"
         << "    \"fragment_bytes\": " << (statvfs_ok ? filesystem.f_frsize : 0) << ",\n"
         << "    \"available_bytes\": "
         << (statvfs_ok ? static_cast<std::uint64_t>(filesystem.f_bavail) * filesystem.f_frsize : 0) << "\n"
         << "  },\n"
         << "  \"scenario\": {\"name\": \"" << JsonEscape(scenario.name) << "\", \"family\": \""
         << JsonEscape(scenario.family) << "\", \"cache_condition\": \""
         << CacheConditionName(scenario.cache_condition) << "\", \"rows\": " << scenario.rows
         << ", \"cache_bytes\": " << scenario.cache_bytes << "},\n"
         << "  \"arguments\": [";
  for (std::size_t index = 0; index < config.arguments.size(); ++index) {
    output << (index == 0 ? "" : ", ") << '"' << JsonEscape(config.arguments[index]) << '"';
  }
  output << "]\n}\n";
  output.flush();
  if (!output) {
    Fail("cannot finish metadata.json");
  }
}

auto MakeKey(std::size_t row, std::size_t bytes) -> Bytes {
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
  auto key = Bytes(ordered_prefix_bytes - digit_count, '0');
  key.insert(key.end(), digits.begin(), digits.begin() + static_cast<std::ptrdiff_t>(digit_count));
  while (key.size() < bytes) {
    key.push_back(static_cast<char>('a' + (row + key.size()) % 26U));
  }
  return key;
}

auto MakeValue(std::size_t row, std::size_t bytes, std::size_t generation) -> Bytes {
  auto value = Bytes(bytes, static_cast<char>('a' + (row + generation) % 26U));
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

auto DatabaseFileBytes(const std::filesystem::path &path) -> std::uint64_t {
  auto ignored = std::error_code{};
  return std::filesystem::exists(path, ignored) ? std::filesystem::file_size(path, ignored) : 0;
}

auto PersistentBytes(const std::filesystem::path &path) -> std::uint64_t {
  auto bytes = DatabaseFileBytes(path);
  auto ignored = std::error_code{};
  const auto wal_prefix = path.filename().string() + "-wal";
  for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), ignored)) {
    if (entry.path().filename().string().starts_with(wal_prefix) && entry.is_regular_file(ignored)) {
      bytes += entry.file_size(ignored);
    }
  }
  return bytes;
}

auto BenchmarkOptions(const Scenario &scenario) -> Options {
  auto options = Options{};
  options.page_cache_bytes = scenario.cache_bytes;
  const auto transaction_payload =
      CheckedMultiply(scenario.batch, scenario.key_bytes + scenario.value_bytes, "transaction payload");
  options.max_write_transaction_bytes = std::max<std::size_t>(32U << 20U, transaction_payload * 3U);
  options.wal_segment_bytes = 64U << 20U;
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

auto ValueDigest(BytesView value) -> std::uint64_t {
  if (value.empty()) {
    return 0;
  }
  return static_cast<std::uint64_t>(value.size()) * 131U + static_cast<unsigned char>(value.front()) * 17U +
         static_cast<unsigned char>(value.back());
}

}  // namespace tinydb::bench
