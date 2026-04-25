# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
file(GLOB_RECURSE SRC_MOD_FILES
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/**/*.cppm)
message("SOURCES ${SRC_FILES}")

message("MODULES? ${SRC_MOD_FILES}")

find_package(Intl REQUIRED)
find_package(ZLIB REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(libassert CONFIG REQUIRED)
find_package(foonathan_memory CONFIG REQUIRED)
find_package(sqlgen REQUIRED)

# AddCommonTarget(
#   postgresql_bioseq
#   TYPE POSTGRES_EXTENSION
#   CXX 26
#   RELOCATABLE
#   NAME bioseq
#   COMMENT "BioSeqDB PG Extension"
#   VERSION ${INSTALL_VERSION}
#   COMPONENT ${PROJECT_NAME}
#   INCLUDE_DIRS
#     "${CMAKE_CURRENT_LIST_DIR}/../config"
#     "${CMAKE_CURRENT_LIST_DIR}/include"
#     "${CMAKE_CURRENT_LIST_DIR}/include2"
#     "${CMAKE_CURRENT_LIST_DIR}/backend"
#     "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
#     "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
#     "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
#   LINKS
#     safestring_static
#     fmt
#   SOURCES
#     ${SRC_FILES}
#   SUPPORT_CXX_MODULES
#   CXX_MOD_BASE_DIRS
#     "${CMAKE_CURRENT_LIST_DIR}"
#   CXX_MOD_FILES
#     ${SRC_MOD_FILES}
#   INSTALLS
#     sql/bioseq--1.0.sql
# )

# === bench target (unit-test binary for minimap_port + tool_minimap2) ===
# Sources for the minimap2 reference implementation live under
# ../bench/tool_minimap2/** — they are treated as a black box and only
# linked into the test binary, never into the Postgres extension.
file(GLOB_RECURSE A_SRC_FILES
  ${CMAKE_CURRENT_LIST_DIR}/../bench/tool_minimap2/minimap2/*.c
  ${CMAKE_CURRENT_LIST_DIR}/../bench/tool_minimap2/minimap2/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../bench/bench_main.cpp)
# CXX modules: bench-specific (tool_minimap2 wrapper, generator, evaluation,
# plus the minimap2-internal `alignment` module) + the bioseq module tree
# still needed by bioseq's minimap_port.
file(GLOB_RECURSE A_SRC_MOD_FILES
  ${CMAKE_CURRENT_LIST_DIR}/../bench/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/../bench/tool_minimap2/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/**/*.cppm)
file(GLOB_RECURSE A_SRC_TESTS
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/testing.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../bench/*_test.cpp)

list (APPEND A_SRC_TESTS ${A_SRC_FILES})

list(REMOVE_ITEM SRC_FILES ${A_SRC_TESTS})
list(REMOVE_ITEM SRC_MOD_FILES ${A_SRC_TESTS})

AddCommonTargetX(
  bioseq_aligner
  TYPE EXEC
  MAIN "bench_main\.cpp"
  CXX 26
  RELOCATABLE
  NAME bioseq_aligner
  COMMENT "BioSeq Aligner benchmark harness"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../config"
    "${CMAKE_CURRENT_LIST_DIR}/include2"
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/backend"
    "${CMAKE_CURRENT_LIST_DIR}/../bench/tool_minimap2"
    "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  TESTS
    ${A_SRC_TESTS}
  LINKS
    safestring_static
    fmt
    ZLIB::ZLIB
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    sqlgen::sqlgen
  SOURCES
    ${A_SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../bench"
  CXX_MOD_FILES
    ${A_SRC_MOD_FILES}
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)

AddCommonTargetX(
  postgresql_bioseq
  TYPE POSTGRES_EXTENSION
  CXX 26
  RELOCATABLE
  NAME bioseq
  COMMENT "BioSeqDB PG Extension"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../config"
    "${CMAKE_CURRENT_LIST_DIR}/include2"
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/backend"
    "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    ZLIB::ZLIB
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
  SOURCES
    ${SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
  CXX_MOD_FILES
    ${SRC_MOD_FILES}
  INSTALLS
    sql/bioseq--1.0.sql
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)