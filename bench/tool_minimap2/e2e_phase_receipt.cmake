# Isolated, opt-in bundled minimap2 E2E phase-receipt executable.
#
# bench/bench_e2e.cmake is the sole shared entry point that includes this
# fragment, after bioseq_modules_nopg and project package discovery are
# available. The committed minimap2/map.c and sketch.c contain
# unconditional C++26 modules/templates despite their .c suffixes. CXX 26 is
# therefore intentional and exactly mirrors alignment_benchmark.cmake plus the
# LANGUAGE CXX source property applied by bioseq_add_postgresql_extension.

include_guard(GLOBAL)

include("${PROJECT_SOURCE_DIR}/cmake/bioseq_source_inventory.cmake")
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(BIOSEQ_MINIMAP2_E2E_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(BIOSEQ_MINIMAP2_E2E_SOURCE_DIR
    "${BIOSEQ_MINIMAP2_E2E_DIR}/minimap2")

bioseq_collect_authored_sources(
  BIOSEQ_MINIMAP2_E2E_BUNDLED_SOURCES
  BASE_DIRECTORY "${BIOSEQ_MINIMAP2_E2E_SOURCE_DIR}"
  RECURSE
  INCLUDE_PATTERNS "*.c" "*.cpp"
  EXPECTED_COUNT 17)

bioseq_add_postgresql_extension(
  bioseq_minimap2_e2e_phase_receipt
  TYPE       EXEC
  CXX        26
  MAIN       "e2e_phase_receipt_main\\.cpp"
  RELOCATABLE
  NAME       bioseq-minimap2-e2e
  COMMENT    "Bundled minimap2 E2E CLI with phase receipt side channel"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/bioseq"
    "${PROJECT_SOURCE_DIR}/bioseq/backend/pg/include"
    "${PROJECT_SOURCE_DIR}/bioseq/backend"
    "${BIOSEQ_MINIMAP2_E2E_DIR}"
    "${PROJECT_SOURCE_DIR}/vendor/safestringlib/include"
    "${PROJECT_SOURCE_DIR}/vendor/fmtlog"
    "${PROJECT_SOURCE_DIR}/vendor/fmt"
    "${PROJECT_SOURCE_DIR}/vendor/seqan3/include"
  LINKS
    ZLIB::ZLIB
    Threads::Threads
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BIOSEQ_MINIMAP2_E2E_DIR}/e2e_phase_receipt_main.cpp"
    ${BIOSEQ_MINIMAP2_E2E_BUNDLED_SOURCES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${PROJECT_SOURCE_DIR}/bioseq"
    "${PROJECT_SOURCE_DIR}/bench"
  CXX_MOD_FILES
    "${BIOSEQ_MINIMAP2_E2E_DIR}/alignment.cppm"
  HWY 1.3.0)

target_compile_definitions(
  bioseq_minimap2_e2e_phase_receipt
  PRIVATE BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT=1)

# Fail closed if the shared target helper ever stops preserving the compilation
# semantics used by bioseq_benchmark for this same committed source inventory.
foreach(_bioseq_minimap2_e2e_source IN LISTS
        BIOSEQ_MINIMAP2_E2E_BUNDLED_SOURCES)
  get_source_file_property(
    _bioseq_minimap2_e2e_language
    "${_bioseq_minimap2_e2e_source}"
    LANGUAGE)
  if(NOT _bioseq_minimap2_e2e_language STREQUAL "CXX")
    message(FATAL_ERROR
      "instrumented bundled minimap2 source is not configured as CXX: "
      "${_bioseq_minimap2_e2e_source}")
  endif()
endforeach()
unset(_bioseq_minimap2_e2e_language)
unset(_bioseq_minimap2_e2e_source)

get_target_property(
  _bioseq_minimap2_e2e_standard
  bioseq_minimap2_e2e_phase_receipt
  CXX_STANDARD)
if(NOT _bioseq_minimap2_e2e_standard STREQUAL "26")
  message(FATAL_ERROR
    "instrumented bundled minimap2 target must use C++26")
endif()
unset(_bioseq_minimap2_e2e_standard)

# ksw2_batched is intentionally absent: only the audited 17-source bundled
# minimap2 inventory above participates in this executable.
