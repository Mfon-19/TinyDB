cmake_minimum_required(VERSION 3.25)

project(TinyDBDownstreamConsumer LANGUAGES CXX)

find_package(TinyDB CONFIG REQUIRED)

add_executable(tinydb_downstream main.cpp)
target_link_libraries(tinydb_downstream PRIVATE TinyDB::TinyDB)
target_compile_features(tinydb_downstream PRIVATE cxx_std_23)

# A sanitizer-instrumented static library needs the same runtime only in this
# repository's install test. Sanitizers are not an install-time
# requirement of normal TinyDB packages.
if(TINYDB_TEST_SANITIZER STREQUAL "address")
  target_compile_options(tinydb_downstream PRIVATE -fsanitize=address,undefined
                                                    -fno-omit-frame-pointer)
  target_link_options(tinydb_downstream PRIVATE -fsanitize=address,undefined)
elseif(TINYDB_TEST_SANITIZER STREQUAL "thread")
  target_compile_options(tinydb_downstream PRIVATE -fsanitize=thread -fno-omit-frame-pointer -fno-pie)
  target_link_options(tinydb_downstream PRIVATE -fsanitize=thread -no-pie)
endif()
