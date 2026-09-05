cmake_minimum_required(VERSION 3.24)
project(TinyDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_executable(tinydb_cli main.cpp)
target_link_libraries(tinydb_cli PRIVATE tinydb)
tinydb_target_options(tinydb_cli)

add_executable(tinydb_bench EXCLUDE_FROM_ALL benchmarks/benchmark.cpp)
target_link_libraries(tinydb_bench PRIVATE tinydb)
tinydb_target_options(tinydb_bench)

option(TINYDB_BUILD_TESTS "Build the tests" ON)
if(TINYDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
