# IO benchmark wiring. Included after kseq++ has been resolved.

set(IO_BENCH_DIR        "${CMAKE_CURRENT_LIST_DIR}")
set(BIOSEQ_DIR          "${CMAKE_CURRENT_LIST_DIR}/../..")
set(BENCH_FRAMEWORK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../bench_framework")

# Shared reader modules come from bioseq_modules_nopg. The benchmark target
# privately owns only its named logic module and the benchmark-framework BMI.
set(IO_BENCH_MODULE_FILES
  "${IO_BENCH_DIR}/index.cppm"
  "${IO_BENCH_DIR}/common.cppm"
  "${IO_BENCH_DIR}/lazy_expression.cppm"
  "${IO_BENCH_DIR}/readers.cppm"
  "${IO_BENCH_DIR}/writers.cppm")

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
    "${BIOSEQ_DIR}" "${BIOSEQ_DIR}/backend/pg/include"
    "${BIOSEQ_DIR}/backend"
    "${IO_BENCH_DIR}"
    "${BIOSEQ_DIR}/../vendor/safestringlib/include"
    "${BIOSEQ_DIR}/../vendor/fmtlog"
    "${BIOSEQ_DIR}/../vendor/fmt"
    "${BIOSEQ_DIR}/../vendor/seqan3/include"
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
    "${IO_BENCH_DIR}/main.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${IO_BENCH_MODULE_FILES}
  HWY 1.3.0)

target_compile_definitions(bioseq_io_benchmark
  PRIVATE
    "BIOSEQ_IO_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_io_benchmark\""
    "BIOSEQ_IO_DATA_DIR=\"${BIOSEQ_DIR}/../data\""
    "BIOSEQ_IO_BENCH_CONFIG_PATH=\"${IO_BENCH_DIR}/configuration.json\"")

if(APPLE)
    target_link_options(bioseq_io_benchmark PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
