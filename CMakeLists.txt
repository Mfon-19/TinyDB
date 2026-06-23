cmake_minimum_required(VERSION 3.25)

project(
  synarch
  VERSION 0.1.0
  LANGUAGES CXX)

option(SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SYNARCH_ENABLE_CLANG_TIDY "Run clang-tidy while compiling local targets" OFF)
option(SYNARCH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(FetchContent)

add_library(synarch_warnings INTERFACE)
target_compile_options(
  synarch_warnings
  INTERFACE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wextra>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wpedantic>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wshadow>
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wconversion>
    $<$<AND:$<BOOL:${SYNARCH_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<AND:$<BOOL:${SYNARCH_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>)

add_library(synarch_sanitizers INTERFACE)
if(SANITIZE)
  target_compile_options(
    synarch_sanitizers
    INTERFACE
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=address,undefined>
      $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fno-omit-frame-pointer>)
  target_link_options(
    synarch_sanitizers
    INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-fsanitize=address,undefined>)
endif()

set(SYNARCH_CLANG_TIDY_COMMAND "")
if(SYNARCH_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16)
  if(CLANG_TIDY_EXE)
    set(SYNARCH_CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE};--quiet")
  else()
    message(WARNING "SYNARCH_ENABLE_CLANG_TIDY=ON but clang-tidy was not found")
  endif()
endif()

function(synarch_apply_local_tooling target_name)
  target_link_libraries(${target_name} PRIVATE synarch_warnings synarch_sanitizers)
  if(SYNARCH_CLANG_TIDY_COMMAND)
    set_target_properties(${target_name} PROPERTIES CXX_CLANG_TIDY
                                                    "${SYNARCH_CLANG_TIDY_COMMAND}")
  endif()
endfunction()

add_library(synarch src/database.cpp)
add_library(synarch::synarch ALIAS synarch)
target_compile_features(synarch PUBLIC cxx_std_20)
target_include_directories(
  synarch
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
synarch_apply_local_tooling(synarch)

add_executable(synarch_cli src/cli/main.cpp)
set_target_properties(synarch_cli PROPERTIES OUTPUT_NAME synarch)
target_link_libraries(synarch_cli PRIVATE synarch::synarch)
synarch_apply_local_tooling(synarch_cli)

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

  add_executable(synarch_smoke_test tests/smoke_test.cpp)
  target_link_libraries(synarch_smoke_test PRIVATE synarch::synarch GTest::gtest_main)
  synarch_apply_local_tooling(synarch_smoke_test)
  gtest_discover_tests(synarch_smoke_test)
endif()
