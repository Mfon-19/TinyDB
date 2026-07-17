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

struct Scenario final {
  std::string name;
  std::string family;
  Workload workload{Workload::PointRead};
  AccessPattern access{AccessPattern::Uniform};

  std::size_t rows{0};
  std::size_t key_bytes{16};
  std::size_t value_bytes{128};
  std::size_t cache_bytes{8U << 20U};
  std::size_t cache_ratio_numerator{0};
  std::size_t cache_ratio_denominator{1};
  std::size_t trials{15};
  std::size_t warmups{3};
  std::chrono::milliseconds minimum_trial{std::chrono::seconds(1)};

  std::size_t commits{0};
  std::size_t batch{0};
  std::size_t scan_rows{0};
  std::size_t operations{0};
  std::size_t reader_threads{0};
  std::size_t churn_warmup_rounds{3};
  std::size_t churn_measured_rounds{10};
  std::uint64_t target_bytes{0};

  bool overwrite{false};
  bool random_write_order{false};
  bool transaction_scoped_reads{true};
  bool include_missing_reads{false};
  bool copy_values{true};
  bool drop_file_cache{false};
};

struct Config final {
  std::string profile{"standard"};
  std::filesystem::path output;
  std::optional<std::string> family;
  std::optional<std::string> filter;
  std::optional<std::size_t> trials;
  std::optional<std::size_t> warmups;
  std::optional<std::chrono::milliseconds> minimum_trial;
  std::optional<std::size_t> cache_bytes;
  std::uint64_t seed{0x54494E594442ULL};
  bool list_only{false};
  bool ordered{false};
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
  std::string metric;
  std::string unit;
  std::size_t trial{0};
  std::size_t observation{0};
  double value{0};
};

struct Summary final {
  std::string scenario;
  std::string family;
  std::string metric;
  std::string unit;
  std::size_t samples{0};
  double mean{0};
  double standard_deviation{0};
  double minimum{0};
  double p50{0};
  double p95{0};
  double p99{0};
  double maximum{0};
};

class Results final {
 public:
  void Add(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
           std::size_t observation, double value);
  auto Summaries() const -> std::vector<Summary>;
  void Write(const std::filesystem::path &directory) const;

 private:
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
void WriteMetadata(const Config &config, const std::vector<Scenario> &scenarios, const std::filesystem::path &directory,
                   std::chrono::system_clock::time_point started, std::chrono::steady_clock::duration elapsed);

auto MakeDataset(std::size_t rows, std::size_t key_bytes, std::size_t value_bytes) -> Dataset;
auto MakeKey(std::size_t row, std::size_t bytes) -> Bytes;
auto MakeValue(std::size_t row, std::size_t bytes, std::size_t generation) -> Bytes;
auto TemporaryDatabasePath(std::string_view scenario) -> std::filesystem::path;
void RemoveDatabase(const std::filesystem::path &path);
auto DatabaseFileBytes(const std::filesystem::path &path) -> std::uint64_t;
auto PersistentBytes(const std::filesystem::path &path) -> std::uint64_t;
auto BenchmarkOptions(const Scenario &scenario) -> Options;
void StoreDataset(Database &database, const Dataset &data, bool second, const Scenario &scenario);
void DeleteDataset(Database &database, const Dataset &data);
auto ValueDigest(BytesView value) -> std::uint64_t;

void RunScenario(const Scenario &scenario, const Config &config, Results &results);

}  // namespace tinydb::bench
