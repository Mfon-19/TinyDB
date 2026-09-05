include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(b_plus_tree_test b_plus_tree_test.cpp)
target_link_libraries(b_plus_tree_test PRIVATE tinydb GTest::gtest_main)
target_compile_options(b_plus_tree_test PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(b_plus_tree_test)

add_executable(database_test database_test.cpp)
target_link_libraries(database_test PRIVATE tinydb GTest::gtest_main)
target_compile_options(database_test PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(database_test PROPERTIES TIMEOUT 30)

add_executable(wal_test wal_test.cpp)
target_link_libraries(wal_test PRIVATE tinydb GTest::gtest_main)
target_compile_options(wal_test PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(wal_test)

add_executable(durability_test durability_test.cpp)
target_link_libraries(durability_test PRIVATE tinydb GTest::gtest_main)
target_compile_options(durability_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(durability_test PRIVATE -Wl,--wrap=pwrite)
gtest_discover_tests(durability_test PROPERTIES TIMEOUT 30)
