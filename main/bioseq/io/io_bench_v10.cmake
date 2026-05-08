# io_bench_v10.cmake — wiring for the V10 IO bench (`bioseq_io_bench`).
# Replaces the legacy AddCommonTargetX block in `io/build.cmake` for the
# `bioseq_io_bench` target. Included from `io/build.cmake` after kseq++
# has been resolved via FetchContent.

set(IO_BENCH_V10_DIR    "${CMAKE_CURRENT_LIST_DIR}")
set(BIOSEQ_DIR          "${CMAKE_CURRENT_LIST_DIR}/..")
set(BENCH_FRAMEWORK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../bench_framework")

# Module set: framework module + the IO module tree + bioseq sequences
# (BenchAlphabet uses sequence_custom_alphabet) + instrumentation +
# mlogging. The IO bench TU is NOT HWY-bound itself — it imports HWY
# modules transitively through bioseq_io.public_api but does not call
# HWY-namespaced functions directly. Highway support is still required
# at the target level so the IO module's internal SIMD encoders compile.
file(GLOB_RECURSE IO_BENCH_V10_MOD_FILES
  "${IO_BENCH_V10_DIR}/*.cppm"
  "${BIOSEQ_DIR}/backend/common/*.cppm"
  "${BIOSEQ_DIR}/backend/sequences/*.cppm"
  "${BIOSEQ_DIR}/instrumentation/*.cppm"
  "${BENCH_FRAMEWORK_DIR}/index.cppm")

# Drop the Catch2-bound test listener — same reasoning as bench_v10.cmake.
list(FILTER IO_BENCH_V10_MOD_FILES EXCLUDE REGEX ".*/backend/common/testing\\.cppm$")

AddCommonTargetX(
  bioseq_io_bench
  TYPE       EXEC
  CXX        26
  MAIN       "io_bench_v10_main\\.cpp"
  RELOCATABLE
  NAME       bioseq_io_bench
  COMMENT    "BioSeq I/O reader/writer benchmark (v10)"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config"
    "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/include"
    "${BIOSEQ_DIR}/backend"
    "${IO_BENCH_V10_DIR}"
    "${BIOSEQ_DIR}/../safestringlib/include"
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
  SOURCES
    "${IO_BENCH_V10_DIR}/io_bench_v10_main.cpp"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${IO_BENCH_V10_MOD_FILES}
  HWY 1.3.0)

target_compile_definitions(bioseq_io_bench
  PRIVATE
    "BIOSEQ_IO_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_io_bench\""
    "BIOSEQ_IO_DATA_DIR=\"${BIOSEQ_DIR}/../data\""
    "BIOSEQ_IO_BENCH_CONFIG_PATH=\"${IO_BENCH_V10_DIR}/io_bench_v10_configuration.json\"")

if(APPLE)
    target_link_options(bioseq_io_bench PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
