#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tinydb::bench {

inline constexpr auto kStandardPageCacheBytes = std::size_t{16U << 20U};

enum class Workload {
  Portable,
  Concurrent,
  Checkpoint,
  Recovery,
  Churn,
  IoRead,
};

enum class AccessPattern {
  Sequential,
  Uniform,
};

enum class CacheCondition {
  Fresh,
  EngineHot,
  Steady,
  FileCold,
  OsWarm,
};

enum class FixturePolicy {
  Shared,
  Native,
};

enum class MetricDirection {
  Higher,
  Lower,
};

enum class BenchmarkMode {
  Describe,
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
  Workload workload{Workload::Portable};
  std::string portable_workload;
  AccessPattern access{AccessPattern::Uniform};
  CacheCondition cache_condition{CacheCondition::Fresh};
  FixturePolicy fixture_policy{FixturePolicy::Shared};

  std::string primary_metric{"throughput"};
  MetricDirection primary_direction{MetricDirection::Higher};
  double meaningful_difference{0.03};

  std::size_t rows{0};
  std::size_t key_bytes{16};
  std::size_t value_bytes{128};
  std::size_t page_cache_bytes{kStandardPageCacheBytes};
  std::size_t trials{5};
  std::size_t preparation_rounds{0};

  std::size_t commits{0};
  std::size_t batch{0};
  std::size_t scan_rows{0};
  std::size_t operations{0};
  std::size_t reader_threads{0};
  std::size_t churn_warmup_rounds{0};
  std::size_t churn_measured_rounds{0};
  std::uint64_t target_bytes{0};

  bool default_enabled{true};
};

struct Config final {
  BenchmarkMode mode{BenchmarkMode::List};
  std::filesystem::path fixture;
  std::optional<std::string> filter;
  std::optional<std::string> scenario;
  std::optional<std::size_t> page_cache_bytes;
  std::vector<std::string> families;
  std::string dataset_id;
  std::string profile{"standard"};
  std::string semantics{"durable"};
  std::uint64_t seed{0x54494E594442ULL};
  std::size_t trial_index{0};
};

struct Dataset final {
  std::vector<std::string> keys;
  std::vector<std::string> first_values;
  std::vector<std::string> second_values;
  std::uint64_t logical_bytes{0};
};

struct Entry final {
  std::string_view key;
  std::string_view value;
};

struct ScanResult final {
  std::size_t rows{0};
  std::uint64_t digest{0};
};

struct BackendIdentity final {
  std::string name;
  std::string format_family;
  bool tinydb_qualification{false};
  bool always_durable{false};
};

class Backend {
 public:
  virtual ~Backend() = default;

  Backend(const Backend &) = delete;
  auto operator=(const Backend &) -> Backend & = delete;

  virtual void Put(std::span<const Entry> entries) = 0;
  virtual auto Get(std::string_view key) -> std::optional<std::string> = 0;
  virtual void Delete(std::string_view key) = 0;
  virtual auto Scan(std::optional<std::string_view> lower, std::size_t limit) -> ScanResult = 0;
  virtual void FinishReads() noexcept {}
  virtual void StabilizeFixture() = 0;

 protected:
  Backend() = default;
};

struct FileResidency final {
  std::uint64_t file_bytes{0};
  std::uint64_t pages{0};
  std::uint64_t resident_pages{0};
  std::uint64_t resident_bytes{0};

  auto Ratio() const -> double;
};

struct ProcessIo final {
  std::uint64_t read_syscalls{0};
  std::uint64_t write_syscalls{0};
  std::uint64_t storage_read_bytes{0};
  std::uint64_t storage_write_bytes{0};
};

struct ProcessUsage final {
  double user_seconds{0};
  double system_seconds{0};
  std::uint64_t minor_faults{0};
  std::uint64_t major_faults{0};
  std::uint64_t voluntary_context_switches{0};
  std::uint64_t involuntary_context_switches{0};
};

struct ProcessMemory final {
  std::uint64_t resident_bytes{0};
  std::uint64_t proportional_bytes{0};
};

struct StorageUsage final {
  std::uint64_t bytes{0};
  FileResidency residency;
};

struct Sample final {
  std::string scenario;
  std::string family;
  std::uint64_t trial_seed{0};
  std::string dataset_id;
  std::string metric;
  std::string unit;
  SampleScope scope{SampleScope::Trial};
  std::size_t trial{0};
  std::size_t observation{0};
  double value{0};
};

class Results final {
 public:
  Results(std::uint64_t trial_seed, std::string dataset_id);

  void AddTrial(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
                double value);
  void AddObservation(const Scenario &scenario, std::string_view metric, std::string_view unit, std::size_t trial,
                      std::size_t observation, double value);
  void Print() const;

 private:
  void Add(const Scenario &scenario, std::string_view metric, std::string_view unit, SampleScope scope,
           std::size_t trial, std::size_t observation, double value);

  std::uint64_t trial_seed_{0};
  std::string dataset_id_;
  std::vector<Sample> samples_;
};

[[noreturn]] void Fail(std::string_view message);

auto ParseConfig(int argc, char **argv) -> Config;
auto BuildScenarios(const Config &config) -> std::vector<Scenario>;
void PrintIdentity();
void PrintScenarios(const std::vector<Scenario> &scenarios);

auto Identity() -> BackendIdentity;
auto OpenBackend(const std::filesystem::path &root, const Config &config,
                 const Scenario &scenario) -> std::unique_ptr<Backend>;
auto ObserveFileResidency(const std::filesystem::path &path) -> FileResidency;
auto ObserveStorageUsage(const std::filesystem::path &root) -> StorageUsage;
auto AdviseDropFileCache(const std::filesystem::path &path) -> bool;
auto ObserveProcessIo() -> ProcessIo;
auto SubtractProcessIo(const ProcessIo &after, const ProcessIo &before) -> ProcessIo;
auto ObserveProcessUsage() -> ProcessUsage;
auto SubtractProcessUsage(const ProcessUsage &after, const ProcessUsage &before) -> ProcessUsage;
auto ObserveProcessMemory() -> ProcessMemory;
void WarmDatabaseFamily(const std::filesystem::path &database);

auto MakeDataset(std::size_t rows, std::size_t key_bytes, std::size_t value_bytes) -> Dataset;
auto MakeKey(std::size_t row, std::size_t bytes) -> std::string;
auto MakeValue(std::size_t row, std::size_t bytes, std::size_t generation) -> std::string;

void AddPortableScenarios(const Config &config, std::vector<Scenario> &scenarios);
void AddTinyDbPortableScenarios(const Config &config, std::vector<Scenario> &scenarios);
void BuildPortableFixture(const std::filesystem::path &root, const Scenario &scenario, const Config &config);
void RunPortableTrial(const std::filesystem::path &root, const Scenario &scenario, const Config &config,
                      Results &results);
#if defined(KVBENCH_TINYDB)
void BuildTinyDbFixture(const std::filesystem::path &root, const Scenario &scenario);
void RunTinyDbTrial(const std::filesystem::path &root, const Scenario &scenario, const Config &config,
                    Results &results);
#endif

void BuildScenarioFixture(const std::filesystem::path &path, const Scenario &scenario, const Config &config);
void RunTrial(const std::filesystem::path &path, const Scenario &scenario, const Config &config, Results &results);

}  // namespace tinydb::bench
