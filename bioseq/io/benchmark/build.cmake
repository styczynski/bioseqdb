# IO benchmark wiring. Included after kseq++ has been resolved.

include("${PROJECT_SOURCE_DIR}/cmake/bioseq_source_inventory.cmake")

set(BIOSEQ_IO_BENCHMARK_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(BIOSEQ_IO_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(BIOSEQ_IO_BENCHMARK_FRAMEWORK_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../../../bench_framework")

# Shared reader modules come from bioseq_modules_nopg. The benchmark target
# privately owns only its named logic module and the benchmark-framework BMI.
bioseq_collect_authored_sources(
  BIOSEQ_IO_BENCHMARK_MODULES
  BASE_DIRECTORY "${BIOSEQ_IO_BENCHMARK_DIR}"
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS "main.cppm"
  EXPECTED_COUNT 5)

bioseq_add_postgresql_extension(
  bioseq_io_benchmark
  TYPE       EXEC
  CXX        26
  MAIN       "benchmark/main\\.cppm"
  RELOCATABLE
  NAME       bioseq_io_benchmark
  COMMENT    "Bioseq IO reader/writer benchmark"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_IO_COMPONENT_DIR}"
    "${BIOSEQ_IO_COMPONENT_DIR}/backend/pg/include"
    "${BIOSEQ_IO_COMPONENT_DIR}/backend"
    "${BIOSEQ_IO_BENCHMARK_DIR}"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/safestringlib/include"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/fmtlog"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/fmt"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    reflectcpp::reflectcpp
    ZLIB::ZLIB
    libassert::assert
    foonathan_memory
    ftxui::component
    ftxui::dom
    ftxui::screen
    simdjson::simdjson
    nlohmann_json_schema_validator::validator
    TBB::tbb
    lfreist-hwinfo::hwinfo
    kseq++::kseq++
    bioseq_bench_framework
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BIOSEQ_IO_BENCHMARK_DIR}/main.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_IO_COMPONENT_DIR}"
    "${BIOSEQ_IO_BENCHMARK_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BIOSEQ_IO_BENCHMARK_MODULES}
  HWY 1.3.0)

target_compile_definitions(bioseq_io_benchmark
  PRIVATE
    "BIOSEQ_IO_REPORT_DIR=\"${BIOSEQ_IO_COMPONENT_DIR}/../reporting/bioseq_io_benchmark\""
    "BIOSEQ_IO_DATA_DIR=\"${BIOSEQ_IO_COMPONENT_DIR}/../data\""
    "BIOSEQ_IO_BENCH_CONFIG_PATH=\"${BIOSEQ_IO_BENCHMARK_DIR}/configuration.json\"")

if(APPLE)
    target_link_options(bioseq_io_benchmark PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
