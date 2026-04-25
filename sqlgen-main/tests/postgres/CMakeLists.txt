project(sqlgen-postgres-tests)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(
    sqlgen-postgres-tests 
    ${SOURCES}
)
target_precompile_headers(sqlgen-postgres-tests PRIVATE [["sqlgen.hpp"]] <iostream> <string> <functional> <gtest/gtest.h>)

target_link_libraries(sqlgen-postgres-tests PRIVATE sqlgen_tests_crt)

add_custom_command(TARGET sqlgen-postgres-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:sqlgen-postgres-tests> $<TARGET_RUNTIME_DLLS:sqlgen-postgres-tests>
  COMMAND_EXPAND_LISTS
)

find_package(GTest)
gtest_discover_tests(sqlgen-postgres-tests)
