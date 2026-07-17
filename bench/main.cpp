#include "benchmark.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

auto main(int argc, char **argv) -> int {
  using namespace tinydb::bench;

  const auto config = ParseConfig(argc, argv);
  auto scenarios = BuildScenarios(config);
  if (config.list_only) {
    PrintScenarios(scenarios);
    return 0;
  }
  if (!config.ordered) {
    auto generator = std::mt19937_64{config.seed};
    std::ranges::shuffle(scenarios, generator);
  }

  auto error = std::error_code{};
  if (std::filesystem::exists(config.output, error)) {
    Fail("benchmark output directory already exists");
  }
  if (error) {
    Fail("cannot inspect benchmark output path: " + error.message());
  }
  std::filesystem::create_directories(config.output, error);
  if (error) {
    Fail("cannot create benchmark output directory: " + error.message());
  }

  const auto wall_started = std::chrono::system_clock::now();
  const auto steady_started = std::chrono::steady_clock::now();
  auto results = Results{};
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    std::fprintf(stderr, "[%zu/%zu] %s\n", index + 1U, scenarios.size(), scenarios[index].name.c_str());
    RunScenario(scenarios[index], config, results);
  }

  results.Write(config.output);
  WriteMetadata(config, scenarios, config.output, wall_started, std::chrono::steady_clock::now() - steady_started);
  std::printf("Benchmark artifacts: %s\n", config.output.c_str());
  return 0;
}
