# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
file(GLOB_RECURSE SRC_MOD_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm)
message("SOURCES ${SRC_FILES}")

message("MODULES? ${SRC_MOD_FILES}")

find_package(Intl)
find_package(ZLIB)
find_package(reflectcpp CONFIG REQUIRED)

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

file(GLOB_RECURSE A_SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/aligner/*.c ${CMAKE_CURRENT_LIST_DIR}/aligner/**/*.c ${CMAKE_CURRENT_LIST_DIR}/aligner/*.cpp ${CMAKE_CURRENT_LIST_DIR}/aligner/**/*.cpp)
file(GLOB_RECURSE A_SRC_MOD_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/alignment/*.cppm ${CMAKE_CURRENT_LIST_DIR}/backend/common/*.cppm ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/*.cppm)

AddCommonTarget(
  bioseq_aligner
  TYPE EXEC
  CXX 26
  RELOCATABLE
  NAME bioseq_aligner
  COMMENT "BioSeq Aligner"
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
  SOURCES
    ${A_SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
  CXX_MOD_FILES
    ${A_SRC_MOD_FILES}
  BOOST
    program_options regex
  HWY 1.3.0
)
