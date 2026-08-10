# Bioseq benchmark framework — algorithm-agnostic benchmark orchestration.
#
# Included from bioseq/CMakeLists.txt so the find_package(...) calls
# done there (TBB, simdjson, ftxui, nlohmann_json_schema_validator,
# foonathan_memory, libassert, fmt) are already in scope.
#
# Output: a single executable, `bioseq_bench_framework_example`, built
# in TYPE EXEC mode with no Catch2 linkage (the "NEVER" branch of
# AddCommonTargetX).

# Resolve every owned source-tree component from the repository-level CMake
# source root. The including CMake file belongs to the bioseq component, while
# this framework is its repository-level sibling.
set(BIOSEQ_REPOSITORY_ROOT "${PROJECT_SOURCE_DIR}")
set(BENCH_FRAMEWORK_DIR "${BIOSEQ_REPOSITORY_ROOT}/bench_framework")
set(BIOSEQ_DIR "${BIOSEQ_REPOSITORY_ROOT}/bioseq")

# Module file set: the framework interface and the example driver's primary
# module. Instrumentation and logging come from `bioseq_modules_nopg`, so this
# target does not compile duplicate providers.
set(BENCH_FRAMEWORK_MODULE_FILES
  "${BENCH_FRAMEWORK_DIR}/index.cppm"
  "${BENCH_FRAMEWORK_DIR}/example/index.cppm")

# `interactive.cppm` is an implementation unit of `bioseq.bench.framework`
# (declared without `export`). CMake's
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
    "${BIOSEQ_DIR}" "${BIOSEQ_DIR}/backend/pg/include" "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/safestringlib/include"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmtlog"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmt"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/seqan3/include"
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
    # The shared no-PG module set contains the Vulkan shader compiler. Keep
    # this test/demo target aligned with bioseq_bench so importing production
    # parallelism modules cannot leave glslang symbols unresolved.
    Vulkan::Vulkan
    Vulkan::glslang
    spirv-cross-msl
    Vulkan::LayerSettings
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
    "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bench_framework_example\""
    "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR=\"${BENCH_FRAMEWORK_DIR}\""
)

# The same compile definitions also need to flow into the test
# binary; without them the bench_framework module imports succeed
# but anything that pulled in the example's data-dir / report-dir
# macros (none right now, but a future test might) would break.
if(TARGET bioseq_bench_framework_example_tests_exec)
  target_compile_definitions(bioseq_bench_framework_example_tests_exec
    PRIVATE
      "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bench_framework_example\""
      "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR=\"${BENCH_FRAMEWORK_DIR}\""
  )
endif()
