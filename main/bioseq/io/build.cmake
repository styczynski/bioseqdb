# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

message(WARNING "PAPIESZ HENLO IO MODULE '${CMAKE_CURRENT_LIST_DIR}'")

# === bioseq_io_bench target — sequence I/O reader bench/test binary ===
# Sources live under ../io/. Mirrors the bench-target wiring pattern.
file(GLOB_RECURSE IO_BENCH_SRC_FILES
  ${CMAKE_CURRENT_LIST_DIR}/io_bench_main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../backend/common/testing.cppm)

file(GLOB_RECURSE IO_BENCH_SRC_MOD_FILES
  ${CMAKE_CURRENT_LIST_DIR}/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/common/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/common/simd/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/sequences/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/sequences/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/alignment_hwy/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../instrumentation/**/*.cppm)

# === bioseq_io_bench — Sequence I/O reader Catch2 binary ===
AddCommonTargetX(
  bioseq_io_bench
  TYPE TEST_EXEC
  CXX 26
  RELOCATABLE
  NAME bioseq_io_bench
  COMMENT "BioSeq I/O reader benchmark utility"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../config"
    "${CMAKE_CURRENT_LIST_DIR}/../include2"
    "${CMAKE_CURRENT_LIST_DIR}/../include"
    "${CMAKE_CURRENT_LIST_DIR}/../backend"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../safestringlib/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../vendor/fmtlog"
    "${CMAKE_CURRENT_LIST_DIR}/../../vendor/fmt"
    "${CMAKE_CURRENT_LIST_DIR}/../../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    ZLIB::ZLIB
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    ftxui::component
    ftxui::dom
    ftxui::screen
  SOURCES
    ${IO_BENCH_SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/.."
  CXX_MOD_FILES
    ${IO_BENCH_SRC_MOD_FILES}
  HWY 1.3.0
)

target_compile_definitions(bioseq_io_bench
  PRIVATE
    "BIOSEQ_IO_TEST_DATA_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../../data\""
    "BIOSEQ_IO_REPORT_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../../reporting/bioseq_io_bench\""
)

if(APPLE)
    target_link_options(bioseq_io_bench PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++ # TODO: non-hardcoded path
    )
endif()