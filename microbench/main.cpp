#include "storage/page.h"

#include <benchmark/benchmark.h>

#include <string>

auto main(int argc, char **argv) -> int {
  benchmark::AddCustomContext("tinydb_git_commit", TINYDB_MICROBENCH_GIT_COMMIT);
  benchmark::AddCustomContext("tinydb_git_dirty", TINYDB_MICROBENCH_GIT_DIRTY);
  benchmark::AddCustomContext("build_type", TINYDB_MICROBENCH_BUILD_TYPE);
  benchmark::AddCustomContext("compiler", TINYDB_MICROBENCH_COMPILER);
  benchmark::AddCustomContext("page_size", std::to_string(tinydb::PAGE_SIZE));

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
