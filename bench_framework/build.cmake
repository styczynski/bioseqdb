# Bioseq benchmark framework — algorithm-agnostic benchmark orchestration.
#
# Included from bioseq/CMakeLists.txt so the find_package(...) calls
# done there (TBB, simdjson, ftxui, nlohmann_json_schema_validator,
# foonathan_memory, libassert, fmt) are already in scope.
#
# Outputs: reusable `bioseq_bench_framework` module library and the
# `bioseq_bench_framework_example` executable.

# Resolve every owned source-tree component from the repository-level CMake
# source root. The including CMake file belongs to the bioseq component, while
# this framework is its repository-level sibling.
set(BIOSEQ_REPOSITORY_ROOT "${PROJECT_SOURCE_DIR}")
set(BENCH_FRAMEWORK_DIR "${BIOSEQ_REPOSITORY_ROOT}/bench_framework")
set(BENCH_PRODUCTION_DIR "${BIOSEQ_REPOSITORY_ROOT}/bench")
set(BIOSEQ_DIR "${BIOSEQ_REPOSITORY_ROOT}/bioseq")

# Fixed-threshold algorithm selection is consumed by both the production
# benchmark and the framework's production-config contract tests. Keep its
# named module in one reusable owner so neither executable recompiles it.
add_library(bioseq_bench_prefilter_algorithm_selection STATIC)
set_target_properties(bioseq_bench_prefilter_algorithm_selection PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_bench_prefilter_algorithm_selection
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${BENCH_PRODUCTION_DIR}"
    FILES "${BENCH_PRODUCTION_DIR}/prefilter_algorithm_selection.cppm")

# The framework has one reusable module owner. Executables link this target
# instead of compiling the interface and its implementation unit repeatedly.
add_library(bioseq_bench_framework STATIC)
set_target_properties(bioseq_bench_framework PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_bench_framework
  PRIVATE
    "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${BENCH_FRAMEWORK_DIR}"
    FILES "${BENCH_FRAMEWORK_DIR}/index.cppm")
set_source_files_properties(
  "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
  PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    COMPILE_OPTIONS "-x;c++")
target_include_directories(bioseq_bench_framework PUBLIC
  "${BIOSEQ_DIR}"
  "${BIOSEQ_DIR}/backend/pg/include"
  "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmt"
  "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmtlog")
target_link_libraries(bioseq_bench_framework PUBLIC
  bioseq_modules_nopg
  fmt
  libassert::assert
  foonathan_memory
  ftxui::component
  ftxui::dom
  ftxui::screen
  simdjson::simdjson
  nlohmann_json_schema_validator::validator
  TBB::tbb)

# The executable owns only the example driver's primary module.
set(BENCH_FRAMEWORK_MODULE_FILES
  "${BENCH_FRAMEWORK_DIR}/example/index.cppm")

# The example executable retains only its narrow global launcher.
# source; the framework implementation unit belongs to the shared owner.
set(BENCH_FRAMEWORK_EXAMPLE_SOURCES
  "${BENCH_FRAMEWORK_DIR}/example/main.cppm")

# Catch2 contract tests use an explicit semantic target name. The framework
# links Catch2WithMain into that binary, so index_test.cpp must not define its
# own main. Run with:
#   ninja bioseq_benchmark_framework_contract_tests
#   ./bioseq_benchmark_framework_contract_tests
set(BENCH_FRAMEWORK_TEST_SOURCES
  "${BENCH_FRAMEWORK_DIR}/tests/contract/index_test.cpp")

set(BENCH_FRAMEWORK_EXECUTABLE_INCLUDE_DIRS
  "${BIOSEQ_DIR}"
  "${BIOSEQ_DIR}/backend/pg/include"
  "${BIOSEQ_REPOSITORY_ROOT}/vendor/safestringlib/include"
  "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmtlog"
  "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmt"
  "${BIOSEQ_REPOSITORY_ROOT}/vendor/seqan3/include")
set(BENCH_FRAMEWORK_EXECUTABLE_LINKS
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
  # these targets aligned with bioseq_benchmark so importing production
  # parallelism modules cannot leave glslang symbols unresolved.
  Vulkan::Vulkan
  Vulkan::glslang
  spirv-cross-msl
  Vulkan::LayerSettings
  bioseq_bench_framework)

bioseq_add_postgresql_extension(
  bioseq_bench_framework_example
  TYPE       EXEC
  CXX        26
  MAIN       "main\\.cppm"
  RELOCATABLE
  NAME       bioseq_bench_framework_example
  COMMENT    "Bench framework demo binary"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    ${BENCH_FRAMEWORK_EXECUTABLE_INCLUDE_DIRS}
  LINKS
    ${BENCH_FRAMEWORK_EXECUTABLE_LINKS}
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    ${BENCH_FRAMEWORK_EXAMPLE_SOURCES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_FRAMEWORK_MODULE_FILES}
)

# The contract executable imports only reusable providers. It must not compile
# the example module or make the production demo inherit Catch2 test support.
bioseq_add_postgresql_extension_variant(
  bioseq_benchmark_framework_contract_tests
  ON
  TYPE       EXEC
  CXX        26
  RELOCATABLE
  COMMENT    "Bench framework contract tests"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    ${BENCH_FRAMEWORK_EXECUTABLE_INCLUDE_DIRS}
  LINKS
    ${BENCH_FRAMEWORK_EXECUTABLE_LINKS}
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  TESTS
    ${BENCH_FRAMEWORK_TEST_SOURCES}
)
set_target_properties(bioseq_benchmark_framework_contract_tests PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)
if(BUILD_TESTING)
  add_test(
    NAME bioseq_benchmark_framework_contract_tests
    COMMAND "$<TARGET_FILE:bioseq_benchmark_framework_contract_tests>"
      "[component:benchmark_framework][layer:contract]")
  set_tests_properties(
    bioseq_benchmark_framework_contract_tests
    PROPERTIES LABELS
      "component:benchmark_framework;layer:contract;feature:configuration;feature:e2e_driver;feature:regression_policy;feature:reporting;feature:scheduling;feature:selection;resource:filesystem")
endif()
add_custom_target(
  test_bioseq_bench_framework_example
  COMMAND "./bioseq_benchmark_framework_contract_tests"
    "--reporter" "console::out=-::colour-mode=ansi"
  DEPENDS bioseq_benchmark_framework_contract_tests
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

target_compile_definitions(bioseq_bench_framework_example
  PRIVATE
    "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bench_framework_example\""
    "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR=\"${BENCH_FRAMEWORK_DIR}\""
)

# The same compile definitions also need to flow into the test
# binary; without them the bench_framework module imports succeed
# but anything that pulled in the example's data-dir / report-dir
# macros (none right now, but a future test might) would break.
if(TARGET bioseq_benchmark_framework_contract_tests)
  target_link_libraries(bioseq_benchmark_framework_contract_tests PRIVATE
    bioseq_bench_prefilter_algorithm_selection)
  target_compile_definitions(bioseq_benchmark_framework_contract_tests
    PRIVATE
      "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bench_framework_example\""
      "BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR=\"${BENCH_FRAMEWORK_DIR}\""
  )
endif()
