cmake_minimum_required(VERSION 3.24)
project(TinyDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION AND
   NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT tinydb_ipo_supported LANGUAGES CXX)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ${tinydb_ipo_supported})
endif()

find_package(Threads REQUIRED)

set(TINYDB_SANITIZER "" CACHE STRING "Sanitizer: empty, address, or thread")
set_property(CACHE TINYDB_SANITIZER PROPERTY STRINGS "" address thread)
if(TINYDB_SANITIZER STREQUAL "address")
  set(tinydb_sanitizer_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(TINYDB_SANITIZER STREQUAL "thread")
  set(tinydb_sanitizer_flags -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT TINYDB_SANITIZER STREQUAL "")
  message(FATAL_ERROR "TINYDB_SANITIZER must be empty, address, or thread")
endif()

function(tinydb_target_options target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion ${tinydb_sanitizer_flags})
  target_link_options(${target} PUBLIC ${tinydb_sanitizer_flags})
endfunction()

add_library(tinydb
  src/api/cursor.cpp
  src/api/database.cpp
  src/api/page_context.cpp
  src/api/read_transaction.cpp
  src/api/write_transaction.cpp
  src/btree/b_plus_tree.cpp
  src/cache/buffer_pool.cpp
  src/storage/crc32.cpp
  src/storage/disk_manager.cpp
  src/storage/file.cpp
  src/storage/page_codec.cpp
  src/storage/superblock_codec.cpp
  src/storage/wal.cpp
  src/storage/wal_codec.cpp)
target_include_directories(tinydb PUBLIC include)
target_compile_features(tinydb PUBLIC cxx_std_23)
target_link_libraries(tinydb PUBLIC Threads::Threads)
tinydb_target_options(tinydb)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  message(FATAL_ERROR "TinyDB requires x86 with AVX-512 and a GCC/Clang compiler")
endif()
set_source_files_properties(src/storage/crc32.cpp PROPERTIES COMPILE_OPTIONS
  "-mavx512f;-mavx512bw;-mavx512vl;-mvpclmulqdq;-mpclmul")

add_executable(tinydb_cli main.cpp)
target_link_libraries(tinydb_cli PRIVATE tinydb)
tinydb_target_options(tinydb_cli)

add_executable(tinydb_example EXCLUDE_FROM_ALL examples/example.cpp)
target_link_libraries(tinydb_example PRIVATE tinydb)
tinydb_target_options(tinydb_example)

add_executable(tinydb_bench EXCLUDE_FROM_ALL benchmarks/benchmark.cpp)
target_link_libraries(tinydb_bench PRIVATE tinydb)
tinydb_target_options(tinydb_bench)

option(TINYDB_BENCH_SQLITE "Enable SQLite benchmark comparisons" OFF)
if(TINYDB_BENCH_SQLITE)
  find_package(SQLite3 3.24 REQUIRED)
  target_link_libraries(tinydb_bench PRIVATE SQLite::SQLite3)
  target_compile_definitions(tinydb_bench PRIVATE TINYDB_BENCH_SQLITE)
endif()

option(TINYDB_SQLITE "Build the SQLite virtual table module" OFF)
if(TINYDB_SQLITE)
  find_package(SQLite3 3.24 REQUIRED)
  # The loadable extension links the library into a shared object.
  set_target_properties(tinydb PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(tinydb_sqlite STATIC sqlite/tinydb_vtab.cpp)
  target_compile_definitions(tinydb_sqlite PRIVATE SQLITE_CORE)
  target_include_directories(tinydb_sqlite PUBLIC sqlite)
  target_link_libraries(tinydb_sqlite PUBLIC tinydb SQLite::SQLite3)
  tinydb_target_options(tinydb_sqlite)

  # Load in the sqlite3 shell with: .load ./tinydb
  add_library(tinydb_sqlite_extension MODULE sqlite/tinydb_vtab.cpp)
  set_target_properties(tinydb_sqlite_extension PROPERTIES
    OUTPUT_NAME tinydb PREFIX "")
  target_include_directories(tinydb_sqlite_extension PRIVATE
    ${SQLite3_INCLUDE_DIRS})
  target_link_libraries(tinydb_sqlite_extension PRIVATE tinydb)
  tinydb_target_options(tinydb_sqlite_extension)
endif()

option(TINYDB_BUILD_TESTS "Build the tests" ON)
if(TINYDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
