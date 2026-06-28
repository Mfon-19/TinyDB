cmake_minimum_required(VERSION 3.25)

project(
  TinyDB
  VERSION 0.1.0
  LANGUAGES CXX)

option(SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TINYDB_ENABLE_CLANG_TIDY "Run clang-tidy while compiling local targets" OFF)
option(TINYDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
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
    set(TINYDB_CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE};--quiet")
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
  include/tinydb/b_plus_tree.h
  include/tinydb/buffer_pool.h
  include/tinydb/disk_manager.h
  include/tinydb/file_header.h
  include/tinydb/page.h
  include/tinydb/storage_engine.h
  src/btree/b_plus_tree.cpp
  src/buffer/buffer_pool.cpp
  src/engine/storage_engine.cpp
  src/storage/disk_manager.cpp)
add_library(TinyDB::TinyDB ALIAS TinyDB)
target_compile_features(TinyDB PUBLIC cxx_std_20)
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
    TinyDB_apply_local_tooling(TinyDB_disk_manager_test)
    gtest_discover_tests(TinyDB_disk_manager_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/buffer_pool_test.cpp")
    add_executable(TinyDB_buffer_pool_test tests/buffer_pool_test.cpp)
    target_link_libraries(TinyDB_buffer_pool_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    TinyDB_apply_local_tooling(TinyDB_buffer_pool_test)
    gtest_discover_tests(TinyDB_buffer_pool_test)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/b_plus_tree_test.cpp")
    add_executable(TinyDB_b_plus_tree_test tests/b_plus_tree_test.cpp)
    target_link_libraries(TinyDB_b_plus_tree_test PRIVATE TinyDB::TinyDB GTest::gtest_main)
    TinyDB_apply_local_tooling(TinyDB_b_plus_tree_test)
    gtest_discover_tests(TinyDB_b_plus_tree_test)
  endif()
endif()
