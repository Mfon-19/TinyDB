#include "benchmark.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>

#ifndef TINYDB_BENCH_GIT_COMMIT
#define TINYDB_BENCH_GIT_COMMIT "unknown"
#endif

#ifndef TINYDB_BENCH_GIT_DIRTY
#define TINYDB_BENCH_GIT_DIRTY "null"
#endif

#ifndef TINYDB_BENCH_BUILD_TYPE
#define TINYDB_BENCH_BUILD_TYPE "unknown"
#endif

namespace tinydb::bench {
namespace {

struct ProfileSettings final {
  std::size_t cache_bytes;
  std::size_t trials;
  std::size_t warmups;
  std::chrono::milliseconds minimum_trial;
  std::size_t commits;
  std::size_t churn_trials;
  std::size_t churn_warmup_rounds;
  std::size_t churn_measured_rounds;
  std::vector<std::uint64_t> lifecycle_bytes;
  std::uint64_t io_bytes;
  std::size_t io_random_reads;
  std::size_t io_trials;
  std::size_t io_warmups;
  bool full_matrix;
};

auto CheckedMultiply(std::size_t left, std::size_t right, std::string_view description) -> std::size_t {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    Fail(std::string(description) + " is too large");
  }
  return left * right;
}

auto Profile(std::string_view name) -> ProfileSettings {
  if (name == "smoke") {
    return ProfileSettings{
        1U << 20U, 3, 1, std::chrono::milliseconds(100), 8, 1, 1, 3, {2U << 20U}, 8U << 20U, 2'048, 3, 1, false};
  }
  if (name == "standard") {
    return ProfileSettings{8U << 20U,
                           15,
                           3,
                           std::chrono::seconds(1),
                           72,
                           3,
                           3,
                           10,
                           {16U << 20U, 64U << 20U, 256U << 20U},
                           128U << 20U,
                           8'192,
                           7,
                           1,
                           true};
  }
  if (name == "soak") {
    return ProfileSettings{32U << 20U,
                           30,
                           5,
                           std::chrono::seconds(5),
                           128,
                           5,
                           10,
                           40,
                           {64U << 20U, 256U << 20U, 1ULL << 30U},
                           512U << 20U,
                           32'768,
                           10,
                           2,
                           true};
  }
  Fail("unknown benchmark profile");
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

auto RowsForBytes(std::uint64_t bytes, std::size_t key_bytes, std::size_t value_bytes) -> std::size_t {
  constexpr auto estimated_page_overhead = std::uint64_t{32};
  const auto row_bytes =
      static_cast<std::uint64_t>(key_bytes) + static_cast<std::uint64_t>(value_bytes) + estimated_page_overhead;
  return static_cast<std::size_t>(std::max<std::uint64_t>(1, bytes / row_bytes));
}

auto RowsForRatio(std::size_t cache_bytes, std::size_t numerator, std::size_t denominator, std::size_t key_bytes,
                  std::size_t value_bytes) -> std::size_t {
  const auto bytes = static_cast<std::uint64_t>(cache_bytes) * numerator / denominator;
  return RowsForBytes(bytes, key_bytes, value_bytes);
}

auto BaseScenario(const ProfileSettings &profile, std::string name, std::string family, Workload workload) -> Scenario {
  auto scenario = Scenario{};
  scenario.name = std::move(name);
  scenario.family = std::move(family);
  scenario.workload = workload;
  scenario.cache_bytes = profile.cache_bytes;
  scenario.trials = profile.trials;
  scenario.warmups = profile.warmups;
  scenario.minimum_trial = profile.minimum_trial;
  scenario.churn_warmup_rounds = profile.churn_warmup_rounds;
  scenario.churn_measured_rounds = profile.churn_measured_rounds;
  return scenario;
}

void AddWriteScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto add = [&](std::string name, std::size_t value_bytes, std::size_t batch, bool overwrite, bool random) {
    auto scenario = BaseScenario(profile, std::move(name), "writes", Workload::Put);
    scenario.value_bytes = value_bytes;
    scenario.commits = profile.commits;
    scenario.batch = batch;
    scenario.rows = CheckedMultiply(scenario.commits, batch, "write rows");
    scenario.overwrite = overwrite;
    scenario.random_write_order = random;
    scenario.access = random ? AccessPattern::Uniform : AccessPattern::Sequential;
    scenarios.push_back(std::move(scenario));
  };

  add("put.insert.sequential.batch1", 128, 1, false, false);
  add("put.insert.random.batch16", 128, 16, false, true);
  add("put.overwrite.random.batch16", 128, 16, true, true);
  add("put.value64k.batch4", 64U << 10U, 4, false, true);
  if (profile.full_matrix) {
    add("put.insert.sequential.batch16", 128, 16, false, false);
    add("put.insert.sequential.batch256", 128, 256, false, false);
    add("put.value32.batch16", 32, 16, false, true);
    add("put.value1k.batch16", 1U << 10U, 16, false, true);
    add("put.value1m.batch1", 1U << 20U, 1, false, true);

    auto key_scenario = BaseScenario(profile, "put.key256.value1k.batch16", "writes", Workload::Put);
    key_scenario.key_bytes = 256;
    key_scenario.value_bytes = 1U << 10U;
    key_scenario.commits = profile.commits;
    key_scenario.batch = 16;
    key_scenario.rows = CheckedMultiply(key_scenario.commits, key_scenario.batch, "wide-key write rows");
    key_scenario.random_write_order = true;
    key_scenario.access = AccessPattern::Uniform;
    scenarios.push_back(std::move(key_scenario));
  }
}

void AddReadScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto add = [&](std::string name, std::size_t ratio_numerator, std::size_t ratio_denominator,
                       std::size_t value_bytes, AccessPattern access, bool missing, bool transaction_scoped) {
    auto scenario = BaseScenario(profile, std::move(name), "reads", Workload::PointRead);
    scenario.value_bytes = value_bytes;
    scenario.rows =
        RowsForRatio(profile.cache_bytes, ratio_numerator, ratio_denominator, scenario.key_bytes, scenario.value_bytes);
    scenario.cache_ratio_numerator = ratio_numerator;
    scenario.cache_ratio_denominator = ratio_denominator;
    scenario.access = access;
    scenario.include_missing_reads = missing;
    scenario.transaction_scoped_reads = transaction_scoped;
    scenarios.push_back(std::move(scenario));
  };

  add("read.engine_hot.transaction", 1, 2, 128, AccessPattern::Uniform, false, true);
  add("read.eviction.uniform", 8, 1, 128, AccessPattern::Uniform, false, true);
  add("read.eviction.hotspot", 8, 1, 128, AccessPattern::Hotspot, false, true);
  add("read.eviction.missing50", 8, 1, 128, AccessPattern::Uniform, true, true);
  add("read.large_value64k", 8, 1, 64U << 10U, AccessPattern::Uniform, false, true);
  if (profile.full_matrix) {
    add("read.engine_hot.database", 1, 2, 128, AccessPattern::Uniform, false, false);

    auto key_scenario = BaseScenario(profile, "read.key256.eviction", "reads", Workload::PointRead);
    key_scenario.key_bytes = 256;
    key_scenario.value_bytes = 1U << 10U;
    key_scenario.rows = RowsForRatio(profile.cache_bytes, 8, 1, key_scenario.key_bytes, key_scenario.value_bytes);
    key_scenario.cache_ratio_numerator = 8;
    scenarios.push_back(std::move(key_scenario));
  }
}

void AddScanScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto rows = RowsForRatio(profile.cache_bytes, profile.full_matrix ? 8U : 2U, 1, 16, 1U << 10U);
  const auto add = [&](std::string name, std::size_t scan_rows, bool copy_values) {
    auto scenario = BaseScenario(profile, std::move(name), "scans", Workload::Scan);
    scenario.rows = rows;
    scenario.cache_ratio_numerator = profile.full_matrix ? 8U : 2U;
    scenario.value_bytes = 1U << 10U;
    scenario.scan_rows = scan_rows;
    scenario.copy_values = copy_values;
    scenarios.push_back(std::move(scenario));
  };

  add("scan.range16.values", 16, true);
  add("scan.full.metadata", 0, false);
  add("scan.full.values", 0, true);
  if (profile.full_matrix) {
    add("scan.range1.values", 1, true);
    add("scan.range256.values", 256, true);
  }
}

void AddMixedScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto add = [&](std::string name, AccessPattern access) {
    auto scenario = BaseScenario(profile, std::move(name), "mixed", Workload::Mixed);
    scenario.rows = RowsForRatio(profile.cache_bytes, profile.full_matrix ? 8U : 2U, 1, 16, 128);
    scenario.cache_ratio_numerator = profile.full_matrix ? 8U : 2U;
    scenario.access = access;
    scenario.commits = profile.commits;
    scenario.batch = 25;
    scenarios.push_back(std::move(scenario));
  };
  add("mixed.80r12u4i4d.uniform", AccessPattern::Uniform);
  if (profile.full_matrix) {
    add("mixed.80r12u4i4d.hotspot", AccessPattern::Hotspot);
  }
}

void AddConcurrentScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto add = [&](std::size_t readers) {
    auto scenario = BaseScenario(profile, "concurrent.writer.readers" + std::to_string(readers), "concurrency",
                                 Workload::Concurrent);
    scenario.rows = RowsForRatio(profile.cache_bytes, profile.full_matrix ? 2U : 1U, 1, 16, 128);
    scenario.cache_ratio_numerator = profile.full_matrix ? 2U : 1U;
    scenario.commits = profile.commits;
    scenario.reader_threads = readers;
    scenario.batch = 16;
    scenarios.push_back(std::move(scenario));
  };
  add(0);
  add(1);
  add(4);
  if (profile.full_matrix) {
    add(8);
  }
}

void AddLifecycleScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  for (const auto bytes : profile.lifecycle_bytes) {
    const auto mebibytes = bytes >> 20U;
    auto checkpoint =
        BaseScenario(profile, "checkpoint." + std::to_string(mebibytes) + "MiB", "checkpoint", Workload::Checkpoint);
    checkpoint.target_bytes = bytes;
    checkpoint.value_bytes = 1U << 10U;
    checkpoint.rows = RowsForBytes(bytes, checkpoint.key_bytes, checkpoint.value_bytes);
    scenarios.push_back(std::move(checkpoint));

    auto recovery =
        BaseScenario(profile, "recovery.os_warm." + std::to_string(mebibytes) + "MiB", "recovery", Workload::Recovery);
    recovery.target_bytes = bytes;
    recovery.value_bytes = 1U << 10U;
    recovery.rows = RowsForBytes(bytes, recovery.key_bytes, recovery.value_bytes);
    scenarios.push_back(std::move(recovery));
  }
}

void AddChurnScenario(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  auto scenario = BaseScenario(profile, "churn.steady_state", "churn", Workload::Churn);
  scenario.rows = RowsForRatio(profile.cache_bytes, profile.full_matrix ? 2U : 1U, 1, 16, 128);
  scenario.cache_ratio_numerator = profile.full_matrix ? 2U : 1U;
  scenario.trials = profile.churn_trials;
  scenario.warmups = 0;
  scenarios.push_back(std::move(scenario));
}

void AddScalingScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto add = [&](std::string ratio, std::size_t numerator, std::size_t denominator) {
    auto scenario = BaseScenario(profile, "scale.get." + std::move(ratio), "scaling", Workload::PointRead);
    scenario.rows = RowsForRatio(profile.cache_bytes, numerator, denominator, 16, 128);
    scenario.cache_ratio_numerator = numerator;
    scenario.cache_ratio_denominator = denominator;
    scenario.access = AccessPattern::Uniform;
    scenarios.push_back(std::move(scenario));
  };
  add("0_5x_cache", 1, 2);
  add("8x_cache", 8, 1);
  if (profile.full_matrix) {
    add("32x_cache", 32, 1);
  }
}

void AddDirectIoScenarios(std::vector<Scenario> &scenarios, const ProfileSettings &profile) {
  const auto mebibytes = profile.io_bytes >> 20U;
  const auto rows = RowsForBytes(profile.io_bytes, 16, 1U << 10U);
  const auto add = [&](std::string operation, AccessPattern access, std::size_t operations) {
    auto scenario =
        BaseScenario(profile, "io." + std::move(operation) + ".cache_dropped." + std::to_string(mebibytes) + "MiB",
                     "direct_io", Workload::IoRead);
    scenario.access = access;
    scenario.rows = rows;
    scenario.value_bytes = 1U << 10U;
    scenario.target_bytes = profile.io_bytes;
    scenario.operations = operations;
    scenario.trials = profile.io_trials;
    scenario.warmups = profile.io_warmups;
    scenario.minimum_trial = std::chrono::milliseconds::zero();
    scenario.drop_file_cache = true;
    scenarios.push_back(std::move(scenario));
  };

  /* A zero operation count asks the workload to stream the complete tree. */
  add("scan", AccessPattern::Sequential, 0);
  add("random", AccessPattern::Uniform, profile.io_random_reads);
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

auto Timestamp(std::chrono::system_clock::time_point time, bool compact) -> std::string {
  const auto raw = std::chrono::system_clock::to_time_t(time);
  auto utc = std::tm{};
  (void)::gmtime_r(&raw, &utc);
  auto buffer = std::array<char, 32>{};
  std::strftime(buffer.data(), buffer.size(), compact ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer.data();
}

auto DefaultOutput(std::string_view profile) -> std::filesystem::path {
  return std::filesystem::path{"benchmark-results"} /
         (Timestamp(std::chrono::system_clock::now(), true) + '-' + std::string(profile));
}

void Usage() {
  std::puts(
      "usage: TinyDB_bench [options]\n"
      "  --profile smoke|standard|soak\n"
      "  --output PATH\n"
      "  --family NAME\n"
      "  --filter SUBSTRING\n"
      "  --trials N\n"
      "  --warmups N\n"
      "  --minimum-trial-ms N\n"
      "  --cache-bytes N\n"
      "  --seed N\n"
      "  --ordered\n"
      "  --list\n");
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
        if (static_cast<unsigned char>(byte) < 0x20U) {
          escaped += '?';
        } else {
          escaped += byte;
        }
    }
  }
  return escaped;
}

auto ReadFirstValue(const std::filesystem::path &path, std::string_view prefix) -> std::string {
  auto input = std::ifstream{path};
  auto line = std::string{};
  while (std::getline(input, line)) {
    if (line.starts_with(prefix)) {
      const auto delimiter = line.find(':');
      if (delimiter != std::string::npos) {
        auto value = line.substr(delimiter + 1U);
        value.erase(0, value.find_first_not_of(" \t"));
        return value;
      }
    }
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

auto Percentile(const std::vector<double> &sorted, double percentile) -> double {
  const auto position = percentile * static_cast<double>(sorted.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const auto fraction = position - static_cast<double>(lower);
  return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
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

  for (auto index = 1; index < argc; ++index) {
    const auto flag = std::string_view{argv[index]};
    if (flag == "--help") {
      Usage();
      std::exit(0);
    }
    if (flag == "--list") {
      config.list_only = true;
      continue;
    }
    if (flag == "--ordered") {
      config.ordered = true;
      continue;
    }
    if (index + 1 >= argc) {
      Fail("missing benchmark option value");
    }
    const auto value = std::string_view{argv[++index]};
    if (flag == "--profile") {
      config.profile = value;
    } else if (flag == "--output") {
      config.output = value;
    } else if (flag == "--family") {
      config.family = value;
    } else if (flag == "--filter") {
      config.filter = value;
    } else if (flag == "--trials") {
      config.trials = AsSize(ParseUnsigned(value, flag), flag);
    } else if (flag == "--warmups") {
      config.warmups = AsSize(ParseUnsigned(value, flag), flag);
    } else if (flag == "--minimum-trial-ms") {
      config.minimum_trial = std::chrono::milliseconds(AsSize(ParseUnsigned(value, flag), flag));
    } else if (flag == "--cache-bytes") {
      config.cache_bytes = AsSize(ParseUnsigned(value, flag), flag);
    } else if (flag == "--seed") {
      config.seed = ParseUnsigned(value, flag);
    } else {
      Fail(std::string("unknown benchmark option: ") + std::string(flag));
    }
  }

  (void)Profile(config.profile);
  if (config.trials == 0 || config.minimum_trial == std::chrono::milliseconds::zero() || config.cache_bytes == 0) {
    Fail("trial, duration, and cache overrides must be nonzero");
  }
  if (config.output.empty()) {
    config.output = DefaultOutput(config.profile);
  }
  return config;
}

auto BuildScenarios(const Config &config) -> std::vector<Scenario> {
  const auto profile = Profile(config.profile);
  auto scenarios = std::vector<Scenario>{};
  AddWriteScenarios(scenarios, profile);
  AddReadScenarios(scenarios, profile);
  AddScanScenarios(scenarios, profile);
  AddMixedScenarios(scenarios, profile);
  AddConcurrentScenarios(scenarios, profile);
  AddLifecycleScenarios(scenarios, profile);
  AddChurnScenario(scenarios, profile);
  AddScalingScenarios(scenarios, profile);
  AddDirectIoScenarios(scenarios, profile);

  for (auto &scenario : scenarios) {
    if (config.trials) {
      scenario.trials = *config.trials;
    }
    if (config.warmups) {
      scenario.warmups = *config.warmups;
      if (scenario.workload == Workload::Churn) {
        scenario.churn_warmup_rounds = *config.warmups;
      }
    }
    if (config.minimum_trial) {
      scenario.minimum_trial = *config.minimum_trial;
    }
    if (config.cache_bytes) {
      scenario.cache_bytes = *config.cache_bytes;
      if (scenario.cache_ratio_numerator != 0) {
        scenario.rows = RowsForRatio(scenario.cache_bytes, scenario.cache_ratio_numerator,
                                     scenario.cache_ratio_denominator, scenario.key_bytes, scenario.value_bytes);
      }
    }
  }

  std::erase_if(scenarios, [&](const Scenario &scenario) {
    const auto wrong_family = config.family && scenario.family != *config.family;
    const auto wrong_filter = config.filter && !scenario.name.contains(*config.filter);
    return wrong_family || wrong_filter;
  });
  if (scenarios.empty()) {
    Fail("no scenarios match the requested selection");
  }
  return scenarios;
}

void PrintScenarios(const std::vector<Scenario> &scenarios) {
  std::puts(
      "scenario,family,workload,access,rows,key_bytes,value_bytes,cache_bytes,trials,warmups,minimum_trial_ms,"
      "commits,batch,scan_rows,operations,reader_threads,target_bytes,drop_file_cache");
  for (const auto &scenario : scenarios) {
    std::printf("%s,%s,%.*s,%.*s,%zu,%zu,%zu,%zu,%zu,%zu,%lld,%zu,%zu,%zu,%zu,%zu,%llu,%s\n", scenario.name.c_str(),
                scenario.family.c_str(), static_cast<int>(WorkloadName(scenario.workload).size()),
                WorkloadName(scenario.workload).data(), static_cast<int>(AccessName(scenario.access).size()),
                AccessName(scenario.access).data(), scenario.rows, scenario.key_bytes, scenario.value_bytes,
                scenario.cache_bytes, scenario.trials, scenario.warmups,
                static_cast<long long>(scenario.minimum_trial.count()), scenario.commits, scenario.batch,
                scenario.scan_rows, scenario.operations, scenario.reader_threads,
                static_cast<unsigned long long>(scenario.target_bytes), scenario.drop_file_cache ? "true" : "false");
  }
}

void Results::Add(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
                  std::size_t observation, double value) {
  samples_.push_back(
      Sample{scenario.name, scenario.family, std::string(metric), std::string(unit), trial, observation, value});
}

auto Results::Summaries() const -> std::vector<Summary> {
  using Key = std::tuple<std::string, std::string, std::string, std::string>;
  auto groups = std::map<Key, std::vector<double>>{};
  for (const auto &sample : samples_) {
    groups[{sample.scenario, sample.family, sample.metric, sample.unit}].push_back(sample.value);
  }

  auto summaries = std::vector<Summary>{};
  summaries.reserve(groups.size());
  for (auto &[key, values] : groups) {
    std::ranges::sort(values);
    const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    auto squared_difference = 0.0;
    for (const auto value : values) {
      const auto difference = value - mean;
      squared_difference += difference * difference;
    }
    const auto deviation =
        values.size() > 1U ? std::sqrt(squared_difference / static_cast<double>(values.size() - 1U)) : 0.0;
    summaries.push_back(Summary{
        .scenario = std::get<0>(key),
        .family = std::get<1>(key),
        .metric = std::get<2>(key),
        .unit = std::get<3>(key),
        .samples = values.size(),
        .mean = mean,
        .standard_deviation = deviation,
        .minimum = values.front(),
        .p50 = Percentile(values, 0.50),
        .p95 = Percentile(values, 0.95),
        .p99 = Percentile(values, 0.99),
        .maximum = values.back(),
    });
  }
  return summaries;
}

void Results::Write(const std::filesystem::path &directory) const {
  auto samples = std::ofstream{directory / "samples.csv"};
  if (!samples) {
    Fail("cannot open samples.csv");
  }
  samples << "scenario,family,metric,unit,trial,observation,value\n";
  samples << std::setprecision(17);
  for (const auto &sample : samples_) {
    samples << sample.scenario << ',' << sample.family << ',' << sample.metric << ',' << sample.unit << ','
            << sample.trial << ',' << sample.observation << ',' << sample.value << '\n';
  }
  samples.flush();
  if (!samples) {
    Fail("cannot finish samples.csv");
  }

  auto summary = std::ofstream{directory / "summary.csv"};
  if (!summary) {
    Fail("cannot open summary.csv");
  }
  summary << "scenario,family,metric,unit,samples,mean,stddev,min,p50,p95,p99,max\n";
  summary << std::setprecision(17);
  for (const auto &row : Summaries()) {
    summary << row.scenario << ',' << row.family << ',' << row.metric << ',' << row.unit << ',' << row.samples << ','
            << row.mean << ',' << row.standard_deviation << ',' << row.minimum << ',' << row.p50 << ',' << row.p95
            << ',' << row.p99 << ',' << row.maximum << '\n';
  }
  summary.flush();
  if (!summary) {
    Fail("cannot finish summary.csv");
  }
}

void WriteMetadata(const Config &config, const std::vector<Scenario> &scenarios, const std::filesystem::path &directory,
                   std::chrono::system_clock::time_point started, std::chrono::steady_clock::duration elapsed) {
  auto system = utsname{};
  const auto uname_ok = ::uname(&system) == 0;
  const auto fixture_directory = std::filesystem::temp_directory_path();
  struct statvfs filesystem {};
  const auto statvfs_ok = ::statvfs(fixture_directory.c_str(), &filesystem) == 0;
  struct statfs filesystem_type {};
  const auto statfs_ok = ::statfs(fixture_directory.c_str(), &filesystem_type) == 0;
  auto output = std::ofstream{directory / "metadata.json"};
  if (!output) {
    Fail("cannot open metadata.json");
  }

  output << "{\n"
         << "  \"suite_version\": 3,\n"
         << "  \"profile\": \"" << JsonEscape(config.profile) << "\",\n"
         << "  \"seed\": " << config.seed << ",\n"
         << "  \"started_utc\": \"" << Timestamp(started, false) << "\",\n"
         << "  \"elapsed_seconds\": " << std::chrono::duration<double>(elapsed).count() << ",\n"
         << "  \"git_commit\": \"" << TINYDB_BENCH_GIT_COMMIT << "\",\n"
         << "  \"git_dirty\": " << TINYDB_BENCH_GIT_DIRTY << ",\n"
         << "  \"build_type\": \"" << JsonEscape(TINYDB_BENCH_BUILD_TYPE) << "\",\n"
         << "  \"compiler\": \"" << JsonEscape(Compiler()) << "\",\n"
         << "  \"system\": {\n"
         << "    \"hostname\": \"" << JsonEscape(uname_ok ? system.nodename : "unknown") << "\",\n"
         << "    \"os\": \"" << JsonEscape(uname_ok ? system.sysname : "unknown") << "\",\n"
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
         << "    \"total_bytes\": "
         << (statvfs_ok ? static_cast<std::uint64_t>(filesystem.f_blocks) * filesystem.f_frsize : 0) << ",\n"
         << "    \"available_bytes\": "
         << (statvfs_ok ? static_cast<std::uint64_t>(filesystem.f_bavail) * filesystem.f_frsize : 0) << "\n"
         << "  },\n"
         << "  \"cache_terms\": {\n"
         << "    \"engine_hot\": \"working set fits in TinyDB's configured page cache\",\n"
         << "    \"eviction\": \"working set exceeds TinyDB's configured page cache\",\n"
         << "    \"recovery_os_warm\": \"process restart without an OS page-cache eviction claim\",\n"
         << "    \"cache_dropped\": \"POSIX_FADV_DONTNEED was requested; pre-residency records what Linux actually "
            "evicted\"\n"
         << "  },\n"
         << "  \"arguments\": [";
  for (std::size_t index = 0; index < config.arguments.size(); ++index) {
    output << (index == 0 ? "" : ", ") << '"' << JsonEscape(config.arguments[index]) << '"';
  }
  output << "],\n  \"scenarios\": [\n";
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto &scenario = scenarios[index];
    const auto logical_bytes = static_cast<std::uint64_t>(scenario.rows) *
                               static_cast<std::uint64_t>(scenario.key_bytes + scenario.value_bytes);
    output << "    {\"name\": \"" << JsonEscape(scenario.name) << "\", \"family\": \"" << JsonEscape(scenario.family)
           << "\", \"workload\": \"" << WorkloadName(scenario.workload) << "\", \"access\": \""
           << AccessName(scenario.access) << "\", \"rows\": " << scenario.rows
           << ", \"key_bytes\": " << scenario.key_bytes << ", \"value_bytes\": " << scenario.value_bytes
           << ", \"logical_bytes\": " << logical_bytes << ", \"cache_bytes\": " << scenario.cache_bytes
           << ", \"cache_ratio_numerator\": " << scenario.cache_ratio_numerator
           << ", \"cache_ratio_denominator\": " << scenario.cache_ratio_denominator
           << ", \"trials\": " << scenario.trials << ", \"warmups\": " << scenario.warmups
           << ", \"minimum_trial_ms\": " << scenario.minimum_trial.count() << ", \"commits\": " << scenario.commits
           << ", \"batch\": " << scenario.batch << ", \"scan_rows\": " << scenario.scan_rows
           << ", \"operations\": " << scenario.operations << ", \"reader_threads\": " << scenario.reader_threads
           << ", \"churn_warmup_rounds\": " << scenario.churn_warmup_rounds
           << ", \"churn_measured_rounds\": " << scenario.churn_measured_rounds
           << ", \"target_bytes\": " << scenario.target_bytes
           << ", \"overwrite\": " << (scenario.overwrite ? "true" : "false")
           << ", \"random_write_order\": " << (scenario.random_write_order ? "true" : "false")
           << ", \"transaction_scoped_reads\": " << (scenario.transaction_scoped_reads ? "true" : "false")
           << ", \"include_missing_reads\": " << (scenario.include_missing_reads ? "true" : "false")
           << ", \"copy_values\": " << (scenario.copy_values ? "true" : "false")
           << ", \"drop_file_cache\": " << (scenario.drop_file_cache ? "true" : "false") << '}';
    output << (index + 1U == scenarios.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
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
  for (auto index = std::size_t{0}; index < digit_count; ++index) {
    key.push_back(digits[index]);
  }
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

auto TemporaryDatabasePath(std::string_view scenario) -> std::filesystem::path {
  static auto sequence = std::atomic<std::uint64_t>{0};
  return std::filesystem::temp_directory_path() /
         ("tinydb_bench_" + std::string(scenario) + '_' + std::to_string(::getpid()) + '_' +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".db");
}

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

void DeleteDataset(Database &database, const Dataset &data) {
  constexpr auto batch = std::size_t{256};
  for (std::size_t first = 0; first < data.keys.size(); first += batch) {
    auto write = Take(database.BeginWrite(), "BeginWrite delete fixture");
    for (std::size_t row = first; row < std::min(first + batch, data.keys.size()); ++row) {
      Check(write.Delete(data.keys[row]), "Delete fixture");
    }
    (void)Take(std::move(write).Commit(), "Commit delete fixture");
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
