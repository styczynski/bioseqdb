project(sqlgen-sqlite-tests)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(
    sqlgen-sqlite-tests 
    ${SOURCES}
)
target_precompile_headers(sqlgen-sqlite-tests PRIVATE [["sqlgen.hpp"]] <iostream> <string> <functional> <gtest/gtest.h>)

target_link_libraries(sqlgen-sqlite-tests PRIVATE sqlgen_tests_crt)

add_custom_command(TARGET sqlgen-sqlite-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:sqlgen-sqlite-tests> $<TARGET_RUNTIME_DLLS:sqlgen-sqlite-tests>
  COMMAND_EXPAND_LISTS
)

find_package(GTest)
gtest_discover_tests(sqlgen-sqlite-tests)
