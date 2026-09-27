# End-to-end SQL benchmark target.
# Mirrors the alignment benchmark wiring but links the SQL client and Catch2.
# Resolve every owned source-tree component from the repository-level CMake
# source root. This file is included from bioseq/CMakeLists.txt, but the source
# layout is rooted above that component.
include("${PROJECT_SOURCE_DIR}/cmake/bioseq_source_inventory.cmake")

set(BIOSEQ_REPOSITORY_ROOT "${PROJECT_SOURCE_DIR}")
set(BIOSEQ_DIR "${BIOSEQ_REPOSITORY_ROOT}/bioseq")
set(BIOSEQ_E2E_BENCHMARK_FRAMEWORK_DIR
  "${BIOSEQ_REPOSITORY_ROOT}/bench_framework")
set(BIOSEQ_E2E_BENCHMARK_ROOT "${BIOSEQ_REPOSITORY_ROOT}/bench")

# This helper is intentionally visible only to the E2E benchmark graph. It owns
# the bundled minimap2 receipt ABI and cannot leak into ordinary benchmark or
# production targets.
include(
  "${BIOSEQ_E2E_BENCHMARK_ROOT}/tool_minimap2/e2e_phase_receipt.cmake")

# Module TUs the bench owns directly. The E2E driver lives in its primary
# module; e2e/main.cppm is only the executable launcher. The shared
# NO_POSTGRES modules (IO reader, generator, sequences, instrumentation) arrive
# via SHARED_MODULE_LIBS below. The integration support target owns the SQL
# client module and is linked below.
bioseq_collect_authored_sources(
  BIOSEQ_E2E_BENCHMARK_MODULES
  BASE_DIRECTORY "${BIOSEQ_E2E_BENCHMARK_ROOT}/e2e"
  RECURSE
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS
    "main.cppm"
    "tests/support/index.cppm"
  EXPECTED_COUNT 1)

# The test-support interface is a distinct provider compiled only for the
# contract test executable. Its primary-module bridge is enabled by the target-local
# BIOSEQ_E2E_TESTING_API definition below.
bioseq_collect_authored_sources(
  BIOSEQ_E2E_BENCHMARK_TEST_MODULES
  BASE_DIRECTORY "${BIOSEQ_E2E_BENCHMARK_ROOT}/e2e"
  RECURSE
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS "main.cppm"
  EXPECTED_COUNT 2)

bioseq_add_postgresql_extension(
  bioseq_benchmark_e2e
  TYPE EXEC CXX 26
  MAIN "main\\.cppm"
  RELOCATABLE NAME bioseq_benchmark_e2e
  COMMENT "BioSeq end-to-end SQL (bioseq.load + mmsearch) benchmark"
  VERSION ${INSTALL_VERSION} COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}" "${BIOSEQ_DIR}/backend/pg/include"
    "${BIOSEQ_DIR}/backend"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmtlog" "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmt"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/seqan3/include"
  LINKS
    safestring_static fmt reflectcpp::reflectcpp libassert::assert
    foonathan_memory ftxui::component ftxui::dom ftxui::screen
    simdjson::simdjson nlohmann_json_schema_validator::validator
    TBB::tbb lfreist-hwinfo::hwinfo
    PostgreSQL::PostgreSQL                     # libpq — the SQL client
    sqlgen::sqlgen                             # sqlgen PostgresV2Connection (find_package imported target)
    bioseq_integration_test_support
    Vulkan::Vulkan                             # Vulkan loader (bioseq_modules_nopg uses vulkan/shader_compiler)
    ${BIOSEQ_GLSLANG_STATIC_LIBS}               # GLSL→SPIR-V compiler (shader_compiler.cppm)
    spirv-cross-msl                            # SPIR-V → MSL cross-compiler (MoltenVK path)
    Vulkan::LayerSettings                      # validation-layer settings (Vulkan::Vulkan companion)
    bioseq_bench_framework
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BIOSEQ_E2E_BENCHMARK_ROOT}/e2e/main.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BIOSEQ_E2E_BENCHMARK_ROOT}"
    "${BIOSEQ_E2E_BENCHMARK_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BIOSEQ_E2E_BENCHMARK_MODULES}
  HWY 1.3.0)

add_dependencies(
  bioseq_benchmark_e2e
  bioseq_minimap2_e2e_phase_receipt)

# Compile the production serializer itself into a dedicated Catch2 executable.
# This is deliberately a separate target instead of bioseq_add_postgresql_extension(TESTS):
# the latter changes the production executable's Catch2 main/definitions too.
bioseq_collect_authored_sources(
  BIOSEQ_E2E_BENCHMARK_CONTRACT_TEST_SOURCES
  BASE_DIRECTORY "${BIOSEQ_E2E_BENCHMARK_ROOT}/tests/contract"
  INCLUDE_PATTERNS "*_test.cpp"
  EXPECTED_COUNT 1)
bioseq_add_postgresql_extension(
  bioseq_benchmark_e2e_contract_tests
  TYPE EXEC CXX 26
  RELOCATABLE NAME bioseq_benchmark_e2e_contract_tests
  COMMENT "BioSeq E2E raw serializer contract tests"
  VERSION ${INSTALL_VERSION} COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}" "${BIOSEQ_DIR}/backend/pg/include"
    "${BIOSEQ_DIR}/backend"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmtlog" "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmt"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/seqan3/include"
  LINKS
    safestring_static fmt reflectcpp::reflectcpp libassert::assert
    foonathan_memory ftxui::component ftxui::dom ftxui::screen
    simdjson::simdjson nlohmann_json_schema_validator::validator
    TBB::tbb lfreist-hwinfo::hwinfo
    PostgreSQL::PostgreSQL
    sqlgen::sqlgen
    Catch2::Catch2WithMain
    bioseq_integration_test_support
    Vulkan::Vulkan
    ${BIOSEQ_GLSLANG_STATIC_LIBS}
    spirv-cross-msl
    Vulkan::LayerSettings
    bioseq_bench_framework
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    ${BIOSEQ_E2E_BENCHMARK_CONTRACT_TEST_SOURCES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BIOSEQ_E2E_BENCHMARK_ROOT}"
    "${BIOSEQ_E2E_BENCHMARK_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BIOSEQ_E2E_BENCHMARK_TEST_MODULES}
  HWY 1.3.0)

# Match the project's generated Catch2 targets: ThinLTO can drop TLS init
# wrappers for module-attached logging state in test executables.
set_target_properties(bioseq_benchmark_e2e_contract_tests PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)

if(BUILD_TESTING)
  add_test(
    NAME bioseq_benchmark_e2e_contract_tests
    COMMAND "$<TARGET_FILE:bioseq_benchmark_e2e_contract_tests>"
      "[component:benchmark][layer:contract]")
  set_tests_properties(
    bioseq_benchmark_e2e_contract_tests
    PROPERTIES LABELS
      "component:benchmark;layer:contract;feature:configuration;feature:e2e_driver;feature:reporting;feature:selection")
endif()

# The PostgreSQL target graph already gives this definition to the DSO and
# shared module owners. Give the driver-owned module the same selection so its
# serialized duration-rate contract describes the clock implementation it
# imports. Ordinary builds leave the definition absent.
if(BIOSEQ_BENCHMARK_E2E_INTERVALS)
  target_compile_definitions(bioseq_benchmark_e2e PRIVATE
    BIOSEQ_BENCHMARK_E2E_INTERVALS=1)
  if(TARGET bioseq_benchmark_e2e_contract_tests)
    target_compile_definitions(bioseq_benchmark_e2e_contract_tests PRIVATE
      BIOSEQ_BENCHMARK_E2E_INTERVALS=1)
  endif()
endif()

target_compile_definitions(bioseq_benchmark_e2e PRIVATE
  "BIOSEQ_E2E_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bioseq_benchmark_e2e\""
  "BIOSEQ_E2E_DATA_DIR=\"${BIOSEQ_E2E_BENCHMARK_ROOT}\""
  "BIOSEQ_E2E_CONFIG_PATH=\"${BIOSEQ_E2E_BENCHMARK_ROOT}/bench_e2e_configuration.json\""
  "BIOSEQ_E2E_MINIMAP2_BIN=\"$<TARGET_FILE:bioseq_minimap2_e2e_phase_receipt>\"")

if(TARGET bioseq_benchmark_e2e_contract_tests)
  target_compile_definitions(bioseq_benchmark_e2e_contract_tests PRIVATE
    BIOSEQ_E2E_TESTING_API=1
    "BIOSEQ_E2E_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bioseq_benchmark_e2e\""
    "BIOSEQ_E2E_DATA_DIR=\"${BIOSEQ_E2E_BENCHMARK_ROOT}\""
    "BIOSEQ_E2E_CONFIG_PATH=\"${BIOSEQ_E2E_BENCHMARK_ROOT}/bench_e2e_configuration.json\""
    "BIOSEQ_E2E_MINIMAP2_BIN=\"$<TARGET_FILE:bioseq_minimap2_e2e_phase_receipt>\"")
endif()

if(APPLE)
    target_link_options(bioseq_benchmark_e2e PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
    if(TARGET bioseq_benchmark_e2e_contract_tests)
      target_link_options(bioseq_benchmark_e2e_contract_tests PRIVATE
        -L/opt/homebrew/opt/llvm/lib/c++)
    endif()
endif()
