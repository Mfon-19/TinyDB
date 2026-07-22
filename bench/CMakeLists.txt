cmake_minimum_required(VERSION 3.25)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(TinyDBBenchmark LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  set(TINYDB_ENGINE_SOURCE_DIR "" CACHE PATH "TinyDB engine source tree")
  if(NOT TINYDB_ENGINE_SOURCE_DIR)
    message(FATAL_ERROR "TINYDB_ENGINE_SOURCE_DIR is required")
  endif()
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(TINYDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${TINYDB_ENGINE_SOURCE_DIR}" engine)
endif()

if(NOT TINYDB_ENGINE_SOURCE_DIR)
  set(TINYDB_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
get_filename_component(TINYDB_ENGINE_SOURCE_DIR "${TINYDB_ENGINE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(TINYDB_HARNESS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

function(tinydb_git_identity source prefix)
  execute_process(
    COMMAND git -C "${source}" rev-parse HEAD
    OUTPUT_VARIABLE commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(
    COMMAND git -C "${source}" status --porcelain --untracked-files=normal
    OUTPUT_VARIABLE status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT commit)
    set(commit "unknown")
  endif()
  if(status)
    set(dirty true)
  else()
    set(dirty false)
  endif()
  set(${prefix}_COMMIT "${commit}" PARENT_SCOPE)
  set(${prefix}_DIRTY "${dirty}" PARENT_SCOPE)
endfunction()

tinydb_git_identity("${TINYDB_ENGINE_SOURCE_DIR}" TINYDB_ENGINE)
tinydb_git_identity("${TINYDB_HARNESS_SOURCE_DIR}" TINYDB_HARNESS)

add_executable(
  TinyDB_bench
  benchmark.h
  main.cpp
  support.cpp
  system_metrics.cpp
  system_metrics.h
  workloads.cpp)
target_link_libraries(TinyDB_bench PRIVATE TinyDB::TinyDB)
target_include_directories(TinyDB_bench PRIVATE "${TINYDB_ENGINE_SOURCE_DIR}/src")
target_compile_definitions(
  TinyDB_bench
  PRIVATE TINYDB_BENCH_ENGINE_GIT_COMMIT="${TINYDB_ENGINE_COMMIT}"
          TINYDB_BENCH_ENGINE_GIT_DIRTY="${TINYDB_ENGINE_DIRTY}"
          TINYDB_BENCH_HARNESS_GIT_COMMIT="${TINYDB_HARNESS_COMMIT}"
          TINYDB_BENCH_HARNESS_GIT_DIRTY="${TINYDB_HARNESS_DIRTY}"
          TINYDB_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
if(COMMAND TinyDB_apply_local_tooling)
  TinyDB_apply_local_tooling(TinyDB_bench)
endif()
