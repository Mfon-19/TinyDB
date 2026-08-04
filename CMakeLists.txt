cmake_minimum_required(VERSION 3.25)

project(
  TinyDB
  VERSION 0.1.0
  LANGUAGES CXX)

option(TINYDB_ENABLE_CLANG_TIDY "Run clang-tidy while compiling local targets" OFF)
option(TINYDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(TINYDB_BUILD_BENCHMARKS "Build the workload benchmarks in bench/" OFF)
set(TINYDB_SANITIZER "none" CACHE STRING "Sanitizer: none, address, or thread")
set_property(CACHE TINYDB_SANITIZER PROPERTY STRINGS none address thread)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

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
if(TINYDB_SANITIZER STREQUAL "address")
  target_compile_options(
    TinyDB_sanitizers
    INTERFACE
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=address,undefined>
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-omit-frame-pointer>)
  target_link_options(
    TinyDB_sanitizers
    INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=address,undefined>)
elseif(TINYDB_SANITIZER STREQUAL "thread")
  target_compile_options(
    TinyDB_sanitizers
    INTERFACE
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=thread>
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-omit-frame-pointer>
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-pie>)
  target_link_options(
    TinyDB_sanitizers
    INTERFACE
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=thread>
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-no-pie>)
elseif(NOT TINYDB_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "TINYDB_SANITIZER must be none, address, or thread")
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
  # These are repository build policies, not transitive package dependencies.
  target_link_libraries(
    ${target_name} PRIVATE $<BUILD_INTERFACE:TinyDB_warnings>
                           $<BUILD_INTERFACE:TinyDB_sanitizers>)
  if(TINYDB_CLANG_TIDY_COMMAND)
    set_target_properties(${target_name} PROPERTIES CXX_CLANG_TIDY
                                                    "${TINYDB_CLANG_TIDY_COMMAND}")
  endif()
endfunction()

add_library(
  TinyDB
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
  src/btree/page_source.h
  src/btree/page_view.cpp
  src/btree/page_view.h
  src/btree/tree_cursor.cpp
  src/btree/tree_cursor.h
  src/btree/value.h
  src/btree/value_storage.cpp
  src/btree/value_storage.h
  src/cache/committed_page_cache.cpp
  src/cache/committed_page_cache.h
  src/checkpoint/checkpoint_manager.cpp
  src/checkpoint/checkpoint_manager.h
  src/api/database.cpp
  src/io/file_io.cpp
  src/io/file_io.h
  src/io/testable_posix.cpp
  src/io/testable_posix.h
  src/io/unique_fd.h
  src/recovery/recovery.cpp
  src/recovery/recovery.h
  src/storage/disk_manager.cpp
  src/storage/disk_manager.h
  src/storage/database_uuid.h
  src/storage/encoding.h
  src/storage/page.h
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
  src/txn/transaction_pages.cpp
  src/txn/transaction_pages.h
  src/util/check.h
  src/util/crc32.h
  src/verify/verifier.cpp
  src/verify/verifier.h
  src/wal/wal.cpp
  src/wal/wal.h
  src/wal/wal_codec.cpp
  src/wal/wal_codec.h)
add_library(TinyDB::TinyDB ALIAS TinyDB)
set_property(TARGET TinyDB PROPERTY TINYDB_FORMAT_FAMILY tinydb-v2)
target_sources(
  TinyDB
  PUBLIC FILE_SET
         public_headers
         TYPE
         HEADERS
         BASE_DIRS
         ${CMAKE_CURRENT_SOURCE_DIR}/include
         FILES
         include/tinydb/bytes.h
         include/tinydb/cursor.h
         include/tinydb/database.h
         include/tinydb/options.h
         include/tinydb/stats.h
         include/tinydb/status.h
         include/tinydb/transaction.h)
target_compile_features(TinyDB PUBLIC cxx_std_23)
target_include_directories(
  TinyDB
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
TinyDB_apply_local_tooling(TinyDB)

add_executable(TinyDB_cli src/cli/main.cpp)
set_target_properties(TinyDB_cli PROPERTIES OUTPUT_NAME tinydb)
target_link_libraries(TinyDB_cli PRIVATE TinyDB::TinyDB)
TinyDB_apply_local_tooling(TinyDB_cli)

add_executable(TinyDB_dump tools/tinydb_dump.cpp)
set_target_properties(TinyDB_dump PROPERTIES OUTPUT_NAME tinydb_dump)
target_link_libraries(TinyDB_dump PRIVATE TinyDB::TinyDB)
TinyDB_apply_local_tooling(TinyDB_dump)

set(TINYDB_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/TinyDB)
configure_package_config_file(
  cmake/TinyDBConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/TinyDBConfig.cmake
  INSTALL_DESTINATION ${TINYDB_INSTALL_CMAKEDIR})
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TinyDBConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  TARGETS TinyDB TinyDB_cli TinyDB_dump
  EXPORT TinyDBTargets
  FILE_SET public_headers
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(
  EXPORT TinyDBTargets
  FILE TinyDBTargets.cmake
  NAMESPACE TinyDB::
  DESTINATION ${TINYDB_INSTALL_CMAKEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/TinyDBConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/TinyDBConfigVersion.cmake
        DESTINATION ${TINYDB_INSTALL_CMAKEDIR})

if(BUILD_TESTING)
  include(cmake/TinyDBTesting.cmake)
endif()

if(TINYDB_BUILD_BENCHMARKS)
  set(TINYDB_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  add_subdirectory(bench)
endif()
