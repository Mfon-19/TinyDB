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

# All in-tree unit tests are white-box tests and may include src/ headers. The
# installed-consumer test below is the separate proof that none of those paths
# leak through TinyDB's package interface.
function(TinyDB_add_test test_name)
  set(target_name TinyDB_${test_name}_test)
  add_executable(${target_name} ${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_name}_test.cpp)
  target_link_libraries(${target_name} PRIVATE TinyDB::TinyDB GTest::gtest_main)
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  TinyDB_apply_local_tooling(${target_name})
  # PRE_TEST keeps sanitizer-instrumented binaries out of the build step;
  # discovery then runs in the same environment as the test invocation.
  gtest_discover_tests(${target_name} DISCOVERY_MODE PRE_TEST)
endfunction()

TinyDB_add_test(checkpoint_manager)
TinyDB_add_test(committed_page_cache)
TinyDB_add_test(contract)
TinyDB_add_test(cursor)
TinyDB_add_test(database)
TinyDB_add_test(disk_manager)
TinyDB_add_test(overflow_value)
TinyDB_add_test(page_view)
TinyDB_add_test(read_snapshot)
TinyDB_add_test(reader_gate)
TinyDB_add_test(recovery)
TinyDB_add_test(salvage)
TinyDB_add_test(storage_codec)
TinyDB_add_test(transaction_pages)
TinyDB_add_test(tree_page_source)
TinyDB_add_test(verifier)
TinyDB_add_test(wal)

set(TINYDB_DOWNSTREAM_ROOT ${CMAKE_CURRENT_BINARY_DIR}/downstream-consumer)
add_test(
  NAME TinyDB_installed_consumer
  COMMAND
    ${CMAKE_COMMAND} -DTINYDB_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DTINYDB_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -DTINYDB_CONSUMER_ROOT=${TINYDB_DOWNSTREAM_ROOT}
    -DTINYDB_GENERATOR=${CMAKE_GENERATOR} -DTINYDB_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DTINYDB_SANITIZE=${SANITIZE} -P
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestInstalledConsumer.cmake)
