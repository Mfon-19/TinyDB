cmake_minimum_required(VERSION 3.25)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(TinyDBBenchmark LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  set(KVBENCH_BACKEND "tinydb" CACHE STRING
      "Benchmark backend: tinydb, sqlite, leveldb, or rocksdb")
endif()

if(NOT KVBENCH_BACKEND)
  set(KVBENCH_BACKEND "tinydb")
endif()
set(KVBENCH_ENGINE_REVISION "" CACHE STRING
    "Exact external backend version recorded in benchmark metadata")
get_filename_component(TINYDB_HARNESS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Threads REQUIRED)

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

tinydb_git_identity("${TINYDB_HARNESS_SOURCE_DIR}" TINYDB_HARNESS)

add_executable(
  TinyDB_bench
  backend.cpp
  benchmark.cpp
  benchmark.h
  metrics.cpp
  portable.cpp)

if(KVBENCH_BACKEND STREQUAL "tinydb")
  if(NOT TINYDB_ENGINE_SOURCE_DIR)
    set(TINYDB_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
  endif()
  get_filename_component(TINYDB_ENGINE_SOURCE_DIR "${TINYDB_ENGINE_SOURCE_DIR}" ABSOLUTE)
  if(NOT TARGET TinyDB::TinyDB)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(TINYDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${TINYDB_ENGINE_SOURCE_DIR}" engine)
  endif()
  tinydb_git_identity("${TINYDB_ENGINE_SOURCE_DIR}" TINYDB_ENGINE)
  target_sources(
    TinyDB_bench
    PRIVATE tinydb.cpp)
  target_compile_definitions(TinyDB_bench PRIVATE KVBENCH_TINYDB=1)
  target_include_directories(TinyDB_bench PRIVATE "${TINYDB_ENGINE_SOURCE_DIR}/src")
  target_link_libraries(TinyDB_bench PRIVATE TinyDB::TinyDB)
elseif(KVBENCH_BACKEND STREQUAL "sqlite")
  find_package(SQLite3 REQUIRED)
  target_compile_definitions(TinyDB_bench PRIVATE KVBENCH_SQLITE=1)
  target_link_libraries(TinyDB_bench PRIVATE SQLite::SQLite3)
  set(TINYDB_ENGINE_COMMIT "system-sqlite")
  set(TINYDB_ENGINE_DIRTY false)
elseif(KVBENCH_BACKEND STREQUAL "leveldb")
  find_package(leveldb CONFIG REQUIRED)
  target_compile_definitions(TinyDB_bench PRIVATE KVBENCH_LEVELDB=1)
  target_link_libraries(TinyDB_bench PRIVATE leveldb::leveldb)
  set(TINYDB_ENGINE_COMMIT "leveldb")
  set(TINYDB_ENGINE_DIRTY false)
elseif(KVBENCH_BACKEND STREQUAL "rocksdb")
  find_package(RocksDB CONFIG REQUIRED)
  target_compile_definitions(TinyDB_bench PRIVATE KVBENCH_ROCKSDB=1)
  target_link_libraries(TinyDB_bench PRIVATE RocksDB::rocksdb)
  set(TINYDB_ENGINE_COMMIT "rocksdb")
  set(TINYDB_ENGINE_DIRTY false)
else()
  message(FATAL_ERROR
          "KVBENCH_BACKEND must be tinydb, sqlite, leveldb, or rocksdb")
endif()

if(KVBENCH_ENGINE_REVISION)
  set(TINYDB_ENGINE_COMMIT "${KVBENCH_ENGINE_REVISION}")
endif()

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
