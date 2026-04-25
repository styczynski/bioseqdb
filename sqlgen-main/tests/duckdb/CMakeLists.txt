project(sqlgen-duckdb-tests)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(
    sqlgen-duckdb-tests 
    ${SOURCES}
)
target_precompile_headers(sqlgen-duckdb-tests PRIVATE [["sqlgen.hpp"]] <iostream> <string> <functional> <gtest/gtest.h>)

target_link_libraries(sqlgen-duckdb-tests PRIVATE sqlgen_tests_crt)

add_custom_command(TARGET sqlgen-duckdb-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:sqlgen-duckdb-tests> $<TARGET_RUNTIME_DLLS:sqlgen-duckdb-tests>
  COMMAND_EXPAND_LISTS
)

find_package(GTest)
gtest_discover_tests(sqlgen-duckdb-tests)
