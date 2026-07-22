#pragma once

#include <tinydb/database.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb::bench {

enum class Workload {
  Put,
  PointRead,
  Scan,
  Mixed,
  Concurrent,
  Checkpoint,
  Recovery,
  Churn,
  IoRead,
};

enum class AccessPattern {
  Sequential,
  Uniform,
  Hotspot,
};

enum class CacheCondition {
  Fresh,
  EngineHot,
  Steady,
  FileCold,
  OsWarm,
};

enum class MetricDirection {
  Higher,
  Lower,
};

enum class BenchmarkMode {
  List,
  BuildFixture,
  RunTrial,
};

enum class SampleScope {
  Trial,
  Observation,
};

struct Scenario final {
  std::string name;
  std::string family;
  Workload workload{Workload::PointRead};
  AccessPattern access{AccessPattern::Uniform};
  CacheCondition cache_condition{CacheCondition::Fresh};

  std::string primary_metric{"throughput"};
  MetricDirection primary_direction{MetricDirection::Higher};
  double meaningful_difference{0.03};

  std::size_t rows{0};
  std::size_t key_bytes{16};
  std::size_t value_bytes{128};
  std::size_t cache_bytes{8U << 20U};
  std::size_t cache_ratio_numerator{0};
  std::size_t cache_ratio_denominator{1};
  std::size_t trials{5};
  std::chrono::milliseconds warmup{0};
  std::chrono::milliseconds measurement{750};
  std::size_t preparation_rounds{0};

  std::size_t commits{0};
  std::size_t batch{0};
  std::size_t scan_rows{0};
  std::size_t operations{0};
  std::size_t reader_threads{0};
  std::size_t churn_warmup_rounds{0};
  std::size_t churn_measured_rounds{0};
  std::uint64_t target_bytes{0};

  bool overwrite{false};
  bool random_write_order{false};
  bool transaction_scoped_reads{true};
  bool copy_values{true};
  bool drop_file_cache{false};
};

struct Config final {
  BenchmarkMode mode{BenchmarkMode::List};
  std::filesystem::path output;
  std::filesystem::path fixture;
  std::optional<std::string> family;
  std::optional<std::string> filter;
  std::optional<std::string> scenario;
  std::string fixture_id;
  std::uint64_t seed{0x54494E594442ULL};
  std::size_t trial_index{0};
  std::vector<std::string> arguments;
};

struct Dataset final {
  std::vector<Bytes> keys;
  std::vector<Bytes> first_values;
  std::vector<Bytes> second_values;
  std::uint64_t logical_bytes{0};
};

struct Sample final {
  std::string scenario;
  std::string family;
  std::uint64_t trial_seed{0};
  std::string fixture_id;
  std::string metric;
  std::string unit;
  SampleScope scope{SampleScope::Trial};
  std::size_t trial{0};
  std::size_t observation{0};
  double value{0};
};

class Results final {
 public:
  Results(std::uint64_t trial_seed, std::string fixture_id);

  void AddTrial(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
                double value);
  void AddObservation(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
                      std::size_t observation, double value);
  void Write(const std::filesystem::path &directory) const;

 private:
  void Add(const Scenario &scenario, std::string_view metric, std::string_view unit, SampleScope scope,
           std::size_t trial, std::size_t observation, double value);

  std::uint64_t trial_seed_{0};
  std::string fixture_id_;
  std::vector<Sample> samples_;
};

[[noreturn]] void Fail(std::string_view message);
void Check(const Status &status, std::string_view operation);

template <typename T>
auto Take(Result<T> result, std::string_view operation) -> T {
  if (!result) {
    Fail(std::string(operation) + ": " + result.error().ToString());
  }
  return std::move(*result);
}

auto ParseConfig(int argc, char **argv) -> Config;
auto BuildScenarios(const Config &config) -> std::vector<Scenario>;
void PrintScenarios(const std::vector<Scenario> &scenarios);
void WriteMetadata(const Config &config, const Scenario &scenario, const std::filesystem::path &directory,
                   std::chrono::system_clock::time_point started, std::chrono::steady_clock::duration elapsed);

auto MakeDataset(std::size_t rows, std::size_t key_bytes, std::size_t value_bytes) -> Dataset;
auto MakeKey(std::size_t row, std::size_t bytes) -> Bytes;
auto MakeValue(std::size_t row, std::size_t bytes, std::size_t generation) -> Bytes;
auto DatabaseFileBytes(const std::filesystem::path &path) -> std::uint64_t;
auto PersistentBytes(const std::filesystem::path &path) -> std::uint64_t;
auto BenchmarkOptions(const Scenario &scenario) -> Options;
void StoreDataset(Database &database, const Dataset &data, bool second, const Scenario &scenario);
auto ValueDigest(BytesView value) -> std::uint64_t;

void BuildScenarioFixture(const std::filesystem::path &path, const Scenario &scenario);
void RunTrial(const std::filesystem::path &path, const Scenario &scenario, const Config &config, Results &results);

}  // namespace tinydb::bench
