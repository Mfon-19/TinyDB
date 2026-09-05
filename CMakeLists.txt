cmake_minimum_required(VERSION 3.24)
project(TinyDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(tinydb
  src/api/database.cpp
  src/api/page_context.cpp
  src/api/read_transaction.cpp
  src/api/write_transaction.cpp
  src/btree/b_plus_tree.cpp
  src/cache/buffer_pool.cpp
  src/storage/crc32.cpp
  src/storage/disk_manager.cpp
  src/storage/page_codec.cpp
  src/storage/superblock_codec.cpp
  src/storage/wal.cpp
  src/storage/wal_codec.cpp)
target_include_directories(tinydb PUBLIC include)
target_link_libraries(tinydb PUBLIC Threads::Threads)
target_compile_options(tinydb PRIVATE -Wall -Wextra -Wpedantic)

add_executable(tinydb_cli main.cpp)
target_link_libraries(tinydb_cli PRIVATE tinydb)
target_compile_options(tinydb_cli PRIVATE -Wall -Wextra -Wpedantic)

add_executable(tinydb_bench EXCLUDE_FROM_ALL benchmarks/benchmark.cpp)
target_link_libraries(tinydb_bench PRIVATE tinydb)
target_compile_options(tinydb_bench PRIVATE -Wall -Wextra -Wpedantic)

option(TINYDB_BUILD_TESTS "Build the tests" ON)
if(TINYDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
