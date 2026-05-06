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

# Module file set: framework module + the bioseq_instr module tree the
# framework imports + mlogging.
file(GLOB_RECURSE BENCH_FRAMEWORK_INSTR_MODULES
  "${BIOSEQ_DIR}/instrumentation/*.cppm"
  "${BIOSEQ_DIR}/instrumentation/**/*.cppm")

set(BENCH_FRAMEWORK_MODULE_FILES
  ${BENCH_FRAMEWORK_INSTR_MODULES}
  "${BIOSEQ_DIR}/backend/common/mlogging.cppm"
  "${BENCH_FRAMEWORK_DIR}/index.cppm")

set(BENCH_FRAMEWORK_EXAMPLE_SOURCES
  "${BENCH_FRAMEWORK_DIR}/example_main.cpp")

AddCommonTargetX(
  bioseq_bench_framework_example
  TYPE       EXEC
  CXX        26
  RELOCATABLE
  NAME       bioseq_bench_framework_example
  COMMENT    "Bench framework demo binary"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config"
    "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/include"
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
  SOURCES
    ${BENCH_FRAMEWORK_EXAMPLE_SOURCES}
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
