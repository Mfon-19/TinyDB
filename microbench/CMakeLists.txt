set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
  SYSTEM)

# The Clang presets override __cpp_concepts for TinyDB's C++23 libstdc++
# headers. Google Benchmark runs feature probes as C++11, where that override
# makes libstdc++ expose syntax that the probe cannot parse.
set(TINYDB_MICROBENCH_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  string(REPLACE "-D__cpp_concepts=202002L" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
FetchContent_MakeAvailable(googlebenchmark)
set(CMAKE_CXX_FLAGS "${TINYDB_MICROBENCH_CXX_FLAGS}")

execute_process(
  COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE TINYDB_MICROBENCH_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
execute_process(
  COMMAND git -C "${PROJECT_SOURCE_DIR}" status --porcelain --untracked-files=normal
  OUTPUT_VARIABLE TINYDB_MICROBENCH_STATUS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT TINYDB_MICROBENCH_COMMIT)
  set(TINYDB_MICROBENCH_COMMIT unknown)
endif()
if(TINYDB_MICROBENCH_STATUS)
  set(TINYDB_MICROBENCH_DIRTY true)
else()
  set(TINYDB_MICROBENCH_DIRTY false)
endif()

add_executable(
  TinyDB_microbench
  cache_bench.cpp
  crc32_bench.cpp
  main.cpp
  page_codec_bench.cpp
  page_view_bench.cpp
  value_storage_bench.cpp
  wal_codec_bench.cpp)
set_target_properties(TinyDB_microbench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(TinyDB_microbench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(TinyDB_microbench PRIVATE TinyDB::TinyDB benchmark::benchmark)
target_compile_definitions(
  TinyDB_microbench
  PRIVATE TINYDB_MICROBENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
          TINYDB_MICROBENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
          TINYDB_MICROBENCH_GIT_COMMIT="${TINYDB_MICROBENCH_COMMIT}"
          TINYDB_MICROBENCH_GIT_DIRTY="${TINYDB_MICROBENCH_DIRTY}")
TinyDB_apply_local_tooling(TinyDB_microbench)
