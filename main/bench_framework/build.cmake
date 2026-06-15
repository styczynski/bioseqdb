# bench_framework — generic algorithm-agnostic benchmark orchestration.
#
# Included from main/bioseq/CMakeLists.txt so the find_package(...) calls
# done there (TBB, simdjson, ftxui, nlohmann_json_schema_validator,
# foonathan_memory, libassert, fmt) are already in scope.
#
# Output: a single executable, `bioseq_bench_framework_example`, built
# in TYPE EXEC mode with no Catch2 linkage (the "NEVER" branch of
# AddCommonTargetX).

# Resolve the project sub-paths to absolute, since CMAKE_CURRENT_LIST_DIR
# inside an include()'d script refers to the directory of THIS script
# (main/bench_framework/), not the includer.
set(BENCH_FRAMEWORK_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(BIOSEQ_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../bioseq"
  ABSOLUTE)

# Module file set: ONLY the framework's own interface unit (index.cppm).
# instrumentation/* and mlogging.cppm come from `bioseq_modules_nopg`
# (DOC0269 Phase A) so we don't rebuild them for this target.
set(BENCH_FRAMEWORK_MODULE_FILES
  "${BENCH_FRAMEWORK_DIR}/index.cppm")

# `interactive.cppm` is a *module implementation unit* of `bench_framework`
# (declared `module bench_framework;`, no `export`). CMake's
# `target_sources(... FILE_SET TYPE CXX_MODULES ...)` only accepts module
# interface units / partitions in CXX_MOD_FILES; implementation units go in
# the regular SOURCES list, where they're compiled as part of the same
# module without claiming to provide a new interface unit.
set(BENCH_FRAMEWORK_EXAMPLE_SOURCES
  "${BENCH_FRAMEWORK_DIR}/example_main.cpp"
  "${BENCH_FRAMEWORK_DIR}/interactive.cppm")

# Catch2 unit tests — see index_test.cpp for the test cases.
# The TESTS argument auto-spawns a sibling target called
# `bioseq_bench_framework_example_tests_exec`. The framework links
# Catch2WithMain into that binary, so index_test.cpp must NOT define
# its own main(). Run with:
#   ninja bioseq_bench_framework_example_tests_exec
#   ./bioseq_bench_framework_example_tests_exec
set(BENCH_FRAMEWORK_TEST_SOURCES
  "${BENCH_FRAMEWORK_DIR}/index_test.cpp")

AddCommonTargetX(
  bioseq_bench_framework_example
  TYPE       EXEC
  CXX        26
  MAIN       "example_main\\.cpp"
  RELOCATABLE
  NAME       bioseq_bench_framework_example
  COMMENT    "Bench framework demo binary"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config"
    "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/../safestringlib/include"
    "${BIOSEQ_DIR}/../vendor/fmtlog"
    "${BIOSEQ_DIR}/../vendor/fmt"
    "${BIOSEQ_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    ftxui::component
    ftxui::dom
    ftxui::screen
    simdjson::simdjson
    nlohmann_json_schema_validator::validator
    TBB::tbb
    lfreist-hwinfo::hwinfo
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    ${BENCH_FRAMEWORK_EXAMPLE_SOURCES}
  TESTS
    ${BENCH_FRAMEWORK_TEST_SOURCES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_FRAMEWORK_MODULE_FILES}
)

target_compile_definitions(bioseq_bench_framework_example
  PRIVATE
    "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bench_framework_example\""
    "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR=\"${BENCH_FRAMEWORK_DIR}\""
)

# The same compile definitions also need to flow into the test
# binary; without them the bench_framework module imports succeed
# but anything that pulled in the example's data-dir / report-dir
# macros (none right now, but a future test might) would break.
if(TARGET bioseq_bench_framework_example_tests_exec)
  target_compile_definitions(bioseq_bench_framework_example_tests_exec
    PRIVATE
      "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bench_framework_example\""
      "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR=\"${BENCH_FRAMEWORK_DIR}\""
  )
endif()
