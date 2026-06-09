# bench/bench_e2e.cmake — DOC0286 Part 2. End-to-end SQL benchmark target.
# Mirrors bench_v10.cmake (AddCommonTargetX) but links the SQL client + Catch2.
# Self-define the path vars exactly like bench_v10.cmake:8-10 (do NOT rely on outer
# scope) so this file is `include()`-able from the same top-level CMake that
# includes bench_v10.cmake:
get_filename_component(BIOSEQ_DIR "${CMAKE_CURRENT_LIST_DIR}/../bioseq" ABSOLUTE)
set(BENCH_FRAMEWORK_DIR "${BIOSEQ_DIR}/../bench_framework")
set(BENCH_E2E_DIR "${CMAKE_CURRENT_LIST_DIR}")        # = main/bench

# minimap2 CLI binary the bench shells out to. -D cache override wins; otherwise
# default to PATH `minimap2` (DOC0286 R3). The bench aborts at run time if the
# binary is absent or any command exits non-zero.
if(NOT DEFINED BIOSEQ_MINIMAP2_BIN OR BIOSEQ_MINIMAP2_BIN STREQUAL "")
  set(BIOSEQ_MINIMAP2_BIN "minimap2")
endif()

# Module TUs the bench owns directly. The FASTA/parse helpers are folded into
# bench_e2e_main.cpp (SOURCES, HWY-wrapped) — NOT a module (§11). The shared
# NO_POSTGRES modules (IO reader, generator, sequences, instrumentation) arrive via
# SHARED_MODULE_LIBS below. integration_tests/common.cppm is the SQL client; it
# export-imports testing, so this target must own testing.cppm's BMI too.
set(BENCH_E2E_MOD_FILES
  "${BIOSEQ_DIR}/backend/common/testing.cppm"
  "${BIOSEQ_DIR}/integration_tests/common.cppm"
  "${BENCH_FRAMEWORK_DIR}/index.cppm")

AddCommonTargetX(
  bioseq_bench_e2e
  TYPE EXEC CXX 26
  MAIN "bench_e2e_main\\.cpp"
  RELOCATABLE NAME bioseq_bench_e2e
  COMMENT "BioSeq end-to-end SQL (bioseq.load + mmsearch) benchmark"
  VERSION ${INSTALL_VERSION} COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config" "${BIOSEQ_DIR}/include2" "${BIOSEQ_DIR}/include"
    "${BIOSEQ_DIR}/backend"
    "${BIOSEQ_DIR}/../vendor/fmtlog" "${BIOSEQ_DIR}/../vendor/fmt"
    "${BIOSEQ_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static fmt reflectcpp::reflectcpp libassert::assert
    foonathan_memory ftxui::component ftxui::dom ftxui::screen
    simdjson::simdjson nlohmann_json_schema_validator::validator
    TBB::tbb lfreist-hwinfo::hwinfo
    PostgreSQL::PostgreSQL                     # libpq — the SQL client (NEW vs bench_v10)
    sqlgen::sqlgen                             # sqlgen PostgresV2Connection (find_package imported target)
    Catch2::Catch2                             # common.cppm `export import testing` (ASSERT macro)
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BENCH_E2E_DIR}/bench_e2e_main.cpp"
    "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS "${BIOSEQ_DIR}" "${BENCH_E2E_DIR}" "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_E2E_MOD_FILES}                     # common.cppm + bench index.cppm (fasta helpers are in main.cpp SOURCES)
  HWY 1.3.0)

target_compile_definitions(bioseq_bench_e2e PRIVATE
  "BIOSEQ_E2E_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_bench_e2e_doc0301_async_mps_build\""
  "BIOSEQ_E2E_DATA_DIR=\"${BENCH_E2E_DIR}\""
  "BIOSEQ_E2E_CONFIG_PATH=\"${BENCH_E2E_DIR}/bench_e2e_doc0296_per_sequence_no_index_focus.json\""
  "BIOSEQ_E2E_MINIMAP2_BIN=\"${BIOSEQ_MINIMAP2_BIN}\"")   # default: PATH 'minimap2'

if(APPLE)
    target_link_options(bioseq_bench_e2e PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
