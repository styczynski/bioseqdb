# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

message(WARNING "PAPIESZ HENLO IO MODULE '${CMAKE_CURRENT_LIST_DIR}'")

include(FetchContent)
set(BUILD_TESTING OFF)
FetchContent_Declare(kseq++ GIT_REPOSITORY https://github.com/cartoonist/kseqpp.git GIT_TAG v1.1.2)
FetchContent_MakeAvailable(kseq++)

# kseq++'s upstream CMakeLists hard-requires BZip2 and chains its
# INTERFACE_INCLUDE_DIRECTORIES through the kseq++ target, even though
# kseq++'s actual headers only #include <zlib.h> and <thread>. On macOS
# `find_package(BZip2)` resolves to the Xcode SDK's `usr/include`, which
# we then re-inject via `-isystem` ahead of the `-isysroot` that points
# at the Command Line Tools SDK. That mismatch makes libc++'s `<cctype>`
# fail to locate its own `<ctype.h>` wrapper. Strip kseq++ down to the
# only deps it really uses: its own headers + ZLIB.
set_target_properties(kseq++ PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${kseq++_SOURCE_DIR}/include>"
  INTERFACE_LINK_LIBRARIES
    "$<BUILD_INTERFACE:ZLIB::ZLIB>")

# === bioseq_io_bench target — sequence I/O reader bench/test binary ===
# Sources live under ../io/. Mirrors the bench-target wiring pattern.
file(GLOB_RECURSE IO_BENCH_SRC_FILES
  ${CMAKE_CURRENT_LIST_DIR}/io_bench_main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/io_bench/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../backend/common/testing.cppm)

file(GLOB_RECURSE IO_BENCH_SRC_MOD_FILES
  ${CMAKE_CURRENT_LIST_DIR}/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/compression/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/io_bench/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/common/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/common/simd/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/sequences/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/sequences/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../backend/alignment_hwy/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../instrumentation/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../../bench/bench_scheduler/*.cppm)
# Note: deliberately NOT recursing into bench_scheduler/**/*.cppm —
# that path includes `data_utils/sequence_generator.cppm`, which is a
# second HWY-wrapped module. With two HWY-wrapped modules in the same
# target, clang's C++26 module loader emits Highway's NEON
# `operator==` template instance into both BMIs and then complains
# about a duplicate mangled name when downstream TUs import the
# combined graph. The IO bench's corpus generator (`io_bench/corpus.cppm`)
# does not need the SEAL-style sequence generator, so the data_utils
# subdirectory is omitted.

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
    simdjson::simdjson
    kseq++::kseq++
  SOURCES
    ${IO_BENCH_SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../../bench"
  CXX_MOD_FILES
    ${IO_BENCH_SRC_MOD_FILES}
  HWY 1.3.0
)

target_compile_definitions(bioseq_io_bench
  PRIVATE
    "BIOSEQ_IO_TEST_DATA_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../../data\""
    "BIOSEQ_IO_REPORT_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../../reporting/bioseq_io_bench\""
    "BIOSEQ_IO_THRESHOLDS_PATH=\"${CMAKE_CURRENT_LIST_DIR}/io_regression_thresholds.json\""
    "BIOSEQ_IO_SOURCE_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../..\""
)

if(APPLE)
    target_link_options(bioseq_io_bench PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++ # TODO: non-hardcoded path
    )
endif()