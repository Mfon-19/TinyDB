#include "benchmark.h"

#include <chrono>
#include <cstdio>
#include <filesystem>

auto main(int argc, char **argv) -> int {
  using namespace tinydb::bench;

  const auto config = ParseConfig(argc, argv);
  const auto scenarios = BuildScenarios(config);
  if (config.mode == BenchmarkMode::List) {
    PrintScenarios(scenarios);
    return 0;
  }
  if (scenarios.size() != 1U) {
    Fail("fixture operations require exactly one scenario");
  }
  if (config.mode == BenchmarkMode::BuildFixture) {
    BuildScenarioFixture(config.fixture, scenarios.front());
    return 0;
  }

  auto error = std::error_code{};
  if (std::filesystem::exists(config.output, error)) {
    Fail("benchmark output directory already exists");
  }
  std::filesystem::create_directories(config.output, error);
  if (error) {
    Fail("cannot create benchmark output directory: " + error.message());
  }

  const auto wall_started = std::chrono::system_clock::now();
  const auto steady_started = std::chrono::steady_clock::now();
  auto results = Results{config.seed, config.fixture_id};
  RunTrial(config.fixture, scenarios.front(), config, results);
  results.Write(config.output);
  WriteMetadata(config, scenarios.front(), config.output, wall_started,
                std::chrono::steady_clock::now() - steady_started);
  return 0;
}
