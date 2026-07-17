if(NOT DEFINED TINYDB_SOURCE_DIR OR NOT DEFINED TINYDB_BINARY_DIR OR NOT DEFINED TINYDB_CONSUMER_ROOT)
  message(FATAL_ERROR "installed-consumer test is missing a required path")
endif()

set(install_prefix "${TINYDB_CONSUMER_ROOT}/prefix")
set(consumer_build "${TINYDB_CONSUMER_ROOT}/build")
file(REMOVE_RECURSE "${TINYDB_CONSUMER_ROOT}")

set(install_command "${CMAKE_COMMAND}" --install "${TINYDB_BINARY_DIR}" --prefix "${install_prefix}")
if(TINYDB_BUILD_TYPE)
  list(APPEND install_command --config "${TINYDB_BUILD_TYPE}")
endif()
execute_process(
  COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "TinyDB installation failed:\n${install_output}\n${install_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${TINYDB_SOURCE_DIR}/tests/downstream" -B "${consumer_build}"
          -G "${TINYDB_GENERATOR}" -DCMAKE_BUILD_TYPE=${TINYDB_BUILD_TYPE}
          -DCMAKE_PREFIX_PATH=${install_prefix} -DTINYDB_TEST_SANITIZER=${TINYDB_SANITIZER}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed:\n${configure_output}\n${configure_error}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_build}")
if(TINYDB_BUILD_TYPE)
  list(APPEND build_command --config "${TINYDB_BUILD_TYPE}")
endif()
execute_process(
  COMMAND ${build_command}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed:\n${build_output}\n${build_error}")
endif()
