cmake_minimum_required(VERSION 3.25)

project(
  TinyDB
  VERSION 0.1.0
  LANGUAGES CXX)

option(SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TINYDB_ENABLE_CLANG_TIDY "Run clang-tidy while compiling local targets" OFF)
option(TINYDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(TINYDB_BUILD_BENCHMARKS "Build the workload benchmarks in bench/" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(FetchContent)

add_library(TinyDB_warnings INTERFACE)
target_compile_options(
  TinyDB_warnings
  INTERFACE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wextra>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wpedantic>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wshadow>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wconversion>
    $<$<AND:$<BOOL:${TINYDB_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<AND:$<BOOL:${TINYDB_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>)

add_library(TinyDB_sanitizers INTERFACE)
if(SANITIZE)
  target_compile_options(
    TinyDB_sanitizers
    INTERFACE
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=address,undefined>
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-omit-frame-pointer>)
  target_link_options(
    TinyDB_sanitizers
    INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=address,undefined>)
endif()

set(TINYDB_CLANG_TIDY_COMMAND "")
if(TINYDB_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16)
  if(CLANG_TIDY_EXE)
    # LLVM 18 reports __cpp_concepts=201907 while GCC 13's <expected>
    # requires 202002 before exposing std::expected. The real compiler remains
    # GCC; this only lets clang-tidy parse the libstdc++ C++23 header.
    set(TINYDB_CLANG_TIDY_COMMAND
        "${CLANG_TIDY_EXE};--quiet;--extra-arg=-Wno-builtin-macro-redefined;--extra-arg=-D__cpp_concepts=202002L"
    )
  else()
    message(WARNING "TINYDB_ENABLE_CLANG_TIDY=ON but clang-tidy was not found")
  endif()
endif()

function(TinyDB_apply_local_tooling target_name)
  target_link_libraries(${target_name} PRIVATE TinyDB_warnings TinyDB_sanitizers)
  if(TINYDB_CLANG_TIDY_COMMAND)
    set_target_properties(${target_name} PROPERTIES CXX_CLANG_TIDY
                                                    "${TINYDB_CLANG_TIDY_COMMAND}")
  endif()
endfunction()

add_library(
  TinyDB
  include/tinydb/check.h
  include/tinydb/database_uuid.h
  include/tinydb/disk_manager.h
  include/tinydb/limits.h
  include/tinydb/page.h
  include/tinydb/status.h
  include/tinydb/storage_engine.h
  include/tinydb/unique_fd.h
  include/tinydb/wal.h
  src/btree/b_plus_tree.cpp
  src/btree/b_plus_tree.h
  src/btree/internal_page_builder.cpp
  src/btree/internal_page_builder.h
  src/btree/leaf_page_builder.cpp
  src/btree/leaf_page_builder.h
  src/btree/navigation.cpp
  src/btree/navigation.h
  src/btree/page_format.cpp
  src/btree/page_format.h
  src/btree/page_source.cpp
  src/btree/page_source.h
  src/btree/page_view.cpp
  src/btree/page_view.h
  src/btree/tree_cursor.cpp
  src/btree/tree_cursor.h
  src/cache/committed_page_cache.cpp
  src/cache/committed_page_cache.h
  src/cache/committed_page_source.cpp
  src/cache/committed_page_source.h
  src/engine/storage_engine.cpp
  src/io/syscalls.h
  src/storage/disk_manager.cpp
  src/storage/encoding.h
  src/storage/page_codec.cpp
  src/storage/page_codec.h
  src/storage/superblock.cpp
  src/storage/superblock.h
  src/txn/commit_coordinator.cpp
  src/txn/commit_coordinator.h
  src/txn/contract.h
  src/txn/database_state.h
  src/txn/reader_gate.cpp
  src/txn/reader_gate.h
  src/txn/read_snapshot.cpp
  src/txn/read_snapshot.h
  src/txn/state.h
  src/txn/transaction_pages.cpp
  src/txn/transaction_pages.h
  src/util/crc32.h
  src/wal/wal.cpp
  src/wal/wal_codec.cpp
  src/wal/wal_codec.h)
add_library(TinyDB::TinyDB ALIAS TinyDB)
target_compile_features(TinyDB PUBLIC cxx_std_23)
target_include_directories(
  TinyDB
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
TinyDB_apply_local_tooling(TinyDB)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/main.cpp")
  add_executable(TinyDB_cli src/cli/main.cpp)
  set_target_properties(TinyDB_cli PROPERTIES OUTPUT_NAME TinyDB)
  target_link_libraries(TinyDB_cli PRIVATE TinyDB::TinyDB)
  TinyDB_apply_local_tooling(TinyDB_cli)
endif()

if(BUILD_TESTING)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt
      ON
      CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  include(GoogleTest)

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/disk_manager_test.cpp")
    add_executable(TinyDB_disk_manager_test tests/disk_manager_test.cpp)
    target_link_libraries(TinyDB_disk_manager_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    # White-box tests: the suite uses the private syscall hook and header
    # checksum helpers under src/.
    target_include_directories(TinyDB_disk_manager_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_disk_manager_test)
    gtest_discover_tests(TinyDB_disk_manager_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_engine_test.cpp")
    add_executable(TinyDB_storage_engine_test tests/storage_engine_test.cpp)
    target_link_libraries(TinyDB_storage_engine_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_storage_engine_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_storage_engine_test)
    gtest_discover_tests(TinyDB_storage_engine_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_codec_test.cpp")
    add_executable(TinyDB_storage_codec_test tests/storage_codec_test.cpp)
    target_link_libraries(TinyDB_storage_codec_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_storage_codec_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_storage_codec_test)
    gtest_discover_tests(TinyDB_storage_codec_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/page_view_test.cpp")
    add_executable(TinyDB_page_view_test tests/page_view_test.cpp)
    target_link_libraries(TinyDB_page_view_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_page_view_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_page_view_test)
    gtest_discover_tests(TinyDB_page_view_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/tree_page_source_test.cpp")
    add_executable(TinyDB_tree_page_source_test tests/tree_page_source_test.cpp)
    target_link_libraries(TinyDB_tree_page_source_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_tree_page_source_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_tree_page_source_test)
    gtest_discover_tests(TinyDB_tree_page_source_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_test.cpp")
    add_executable(TinyDB_contract_test tests/contract_test.cpp)
    target_link_libraries(TinyDB_contract_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_contract_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_contract_test)
    gtest_discover_tests(TinyDB_contract_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/reader_gate_test.cpp")
    add_executable(TinyDB_reader_gate_test tests/reader_gate_test.cpp)
    target_link_libraries(TinyDB_reader_gate_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_reader_gate_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_reader_gate_test)
    gtest_discover_tests(TinyDB_reader_gate_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/committed_page_cache_test.cpp")
    add_executable(TinyDB_committed_page_cache_test tests/committed_page_cache_test.cpp)
    target_link_libraries(TinyDB_committed_page_cache_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_committed_page_cache_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_committed_page_cache_test)
    gtest_discover_tests(TinyDB_committed_page_cache_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/read_snapshot_test.cpp")
    add_executable(TinyDB_read_snapshot_test tests/read_snapshot_test.cpp)
    target_link_libraries(TinyDB_read_snapshot_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_read_snapshot_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_read_snapshot_test)
    gtest_discover_tests(TinyDB_read_snapshot_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/transaction_pages_test.cpp")
    add_executable(TinyDB_transaction_pages_test tests/transaction_pages_test.cpp)
    target_link_libraries(TinyDB_transaction_pages_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_transaction_pages_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_transaction_pages_test)
    gtest_discover_tests(TinyDB_transaction_pages_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/wal_test.cpp")
    add_executable(TinyDB_wal_test tests/wal_test.cpp)
    target_link_libraries(TinyDB_wal_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    target_include_directories(TinyDB_wal_test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    TinyDB_apply_local_tooling(TinyDB_wal_test)
    gtest_discover_tests(TinyDB_wal_test)
  endif()
endif()

if(TINYDB_BUILD_BENCHMARKS)
  # SYSTEM keeps the library's headers out of our warning set; the benchmarks
  # themselves still build with the project's -Werror flags.
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING
      OFF
      CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR
      OFF
      CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(TinyDB_workloads_bench bench/workloads_bench.cpp)
  target_link_libraries(TinyDB_workloads_bench PRIVATE TinyDB::TinyDB benchmark::benchmark_main)
  TinyDB_apply_local_tooling(TinyDB_workloads_bench)
endif()
