# bench_v10.cmake — wiring for the V10 bioseq aligner bench
# (`bioseq_bench`). Replaces the legacy AddCommonTargetX block in
# `main/bioseq/CMakeLists.txt` for the `bioseq_bench` target.
#
# Included from `main/bioseq/CMakeLists.txt` after the framework's
# `find_package(...)` calls have been resolved.

get_filename_component(BIOSEQ_DIR "${CMAKE_CURRENT_LIST_DIR}/../bioseq" ABSOLUTE)
set(BENCH_FRAMEWORK_DIR "${BIOSEQ_DIR}/../bench_framework")
set(BENCH_V10_DIR       "${CMAKE_CURRENT_LIST_DIR}")

# Module set: ONLY the bench-private modules. The whole Domain N core
# (alignment_hwy, common-minus-testing, sequences, pipeline, instrumentation)
# is delivered via `bioseq_modules_nopg` (DOC0269 Phase A).
set(BENCH_V10_MOD_FILES
  "${BENCH_FRAMEWORK_DIR}/index.cppm"
  "${BENCH_V10_DIR}/sequence_generator.cppm"
  "${BENCH_V10_DIR}/tool_minimap2/tool_minimap2.cppm"
  "${BENCH_V10_DIR}/tool_minimap2/alignment.cppm"
  "${BENCH_V10_DIR}/tool_minimap2/instr_shim.cppm")

# minimap2 C reference sources are linked alongside the bench TU.
file(GLOB_RECURSE BENCH_V10_C_SRC
  "${BENCH_V10_DIR}/tool_minimap2/minimap2/*.c"
  "${BENCH_V10_DIR}/tool_minimap2/minimap2/*.cpp")

AddCommonTargetX(
  bioseq_bench
  TYPE       EXEC
  CXX        26
  MAIN       "bench_v10_main\\.cpp"
  RELOCATABLE
  NAME       bioseq_bench
  COMMENT    "BioSeq Aligner benchmark harness (v10)"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config"
    "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/include"
    "${BIOSEQ_DIR}/backend"
    "${BENCH_V10_DIR}/tool_minimap2"
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
    "${BENCH_V10_DIR}/bench_v10_main.cpp"
    "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
    ${BENCH_V10_C_SRC}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BENCH_V10_DIR}"
    "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_V10_MOD_FILES}
  HWY 1.3.0)

target_compile_definitions(bioseq_bench
  PRIVATE
    "BIOSEQ_BENCH_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_bench\""
    "BIOSEQ_BENCH_DATA_DIR=\"${BENCH_V10_DIR}\""
    "BIOSEQ_BENCH_CONFIG_PATH=\"${BENCH_V10_DIR}/bench_v10_configuration.json\"")

if(APPLE)
    target_link_options(bioseq_bench PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
