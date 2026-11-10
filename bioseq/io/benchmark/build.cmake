# IO benchmark wiring. Included after kseq++ has been resolved.

include("${PROJECT_SOURCE_DIR}/cmake/bioseq_source_inventory.cmake")

set(BIOSEQ_IO_BENCHMARK_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(BIOSEQ_IO_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(BIOSEQ_IO_BENCHMARK_FRAMEWORK_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../../../bench_framework")

# Shared reader modules come from bioseq_modules_nopg. The benchmark target
# privately owns only its named logic module and the benchmark-framework BMI.
bioseq_collect_authored_sources(
  BIOSEQ_IO_BENCHMARK_MODULES
  BASE_DIRECTORY "${BIOSEQ_IO_BENCHMARK_DIR}"
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS "main.cppm"
  EXPECTED_COUNT 6)

# The baseline is built from the pinned release tree only through the managed
# benchmark target. Generated configure/Makefile products stay in the build tree.
include(ExternalProject)
find_program(BIOSEQ_SAMTOOLS_MAKE NAMES gmake make REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BIOSEQ_SAMTOOLS_ROOT "${CMAKE_BINARY_DIR}/bench/tool_samtools")
set(BIOSEQ_SAMTOOLS_CFLAGS "-O3 -DNDEBUG")
set(BIOSEQ_SAMTOOLS_CONFIGURE_ARGS
  --without-curses --disable-bz2 --disable-lzma --disable-libcurl
  --with-libdeflate --enable-configure-htslib --with-htslib=htslib-1.25)
set(_bioseq_samtools_snapshot_args)
foreach(_argument IN LISTS BIOSEQ_SAMTOOLS_CONFIGURE_ARGS)
  list(APPEND _bioseq_samtools_snapshot_args "--configure-arg=${_argument}")
endforeach()
set(_bioseq_samtools_sysroot "${CMAKE_SYSROOT}")
if(APPLE)
  set(_bioseq_samtools_sysroot "${CMAKE_OSX_SYSROOT}")
endif()
# Added/removed files trigger reconfiguration too; the verifier rejects every
# unmanifested input. Content edits and dependency identities are tracked below.
file(GLOB_RECURSE _bioseq_samtools_inventory CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/bench/tool_samtools/*")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/bioseq_samtools_snapshot.py"
    --source "${PROJECT_SOURCE_DIR}/bench/tool_samtools"
    --root "${BIOSEQ_SAMTOOLS_ROOT}"
    --cc "${CMAKE_C_COMPILER}" --make "${BIOSEQ_SAMTOOLS_MAKE}"
    "--cflags=${BIOSEQ_SAMTOOLS_CFLAGS}"
    --includes "${ZLIB_INCLUDE_DIRS};${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    --libdir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
    --system "${CMAKE_SYSTEM_NAME}:${CMAKE_SYSTEM_PROCESSOR}"
    --sysroot "${_bioseq_samtools_sysroot}"
    ${_bioseq_samtools_snapshot_args}
  RESULT_VARIABLE _bioseq_samtools_snapshot_result)
if(NOT "${_bioseq_samtools_snapshot_result}" STREQUAL "0")
  message(FATAL_ERROR "Pinned samtools snapshot verification failed")
endif()
include("${BIOSEQ_SAMTOOLS_ROOT}/selected.cmake")
# Check the copied upstream inputs at every target build too: a work-tree edit
# is not necessarily a configure dependency. A changed selected identity fails
# this build rather than silently relabeling an already configured project.
add_custom_target(bioseq_samtools_inputs
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/bioseq_samtools_snapshot.py"
    --source "${PROJECT_SOURCE_DIR}/bench/tool_samtools"
    --root "${BIOSEQ_SAMTOOLS_ROOT}"
    --cc "${CMAKE_C_COMPILER}" --make "${BIOSEQ_SAMTOOLS_MAKE}"
    "--cflags=${BIOSEQ_SAMTOOLS_CFLAGS}"
    --includes "${ZLIB_INCLUDE_DIRS};${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    --libdir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
    --system "${CMAKE_SYSTEM_NAME}:${CMAKE_SYSTEM_PROCESSOR}"
    --sysroot "${_bioseq_samtools_sysroot}"
    --expected-build-id "${BIOSEQ_SAMTOOLS_BUILD_ID}"
    ${_bioseq_samtools_snapshot_args}
  VERBATIM)
ExternalProject_Add(bioseq_samtools_baseline
  PREFIX "${BIOSEQ_SAMTOOLS_ROOT}/builds/${BIOSEQ_SAMTOOLS_BUILD_ID}/external"
  SOURCE_DIR "${BIOSEQ_SAMTOOLS_BUILD_DIR}"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
    "CC=${CMAKE_C_COMPILER}" "CFLAGS=${BIOSEQ_SAMTOOLS_CFLAGS}"
    "CPPFLAGS=${BIOSEQ_SAMTOOLS_CPPFLAGS}"
    "LDFLAGS=${BIOSEQ_SAMTOOLS_LDFLAGS}"
    <SOURCE_DIR>/configure ${BIOSEQ_SAMTOOLS_CONFIGURE_ARGS}
  BUILD_COMMAND "${BIOSEQ_SAMTOOLS_MAKE}" -j2 samtools
  BUILD_IN_SOURCE TRUE
  BUILD_BYPRODUCTS "${BIOSEQ_SAMTOOLS_BUILD_DIR}/samtools"
    "${BIOSEQ_SAMTOOLS_BUILD_DIR}/htslib-1.25/libhts.a"
  INSTALL_COMMAND ""
  EXCLUDE_FROM_ALL TRUE)
add_dependencies(bioseq_samtools_baseline bioseq_samtools_inputs)
# The selected digest can return to a previously completed ExternalProject.
# Its install stamp cannot authorize the shared public helper: refresh that
# helper on every managed target build, including A -> B -> A selections.
add_custom_target(bioseq_samtools_runtime
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${BIOSEQ_SAMTOOLS_BUILD_DIR}/samtools" "${BIOSEQ_SAMTOOLS_ROOT}/samtools"
  BYPRODUCTS "${BIOSEQ_SAMTOOLS_ROOT}/samtools"
  DEPENDS bioseq_samtools_baseline
  VERBATIM)
add_library(bioseq_samtools_hts STATIC IMPORTED)
set_target_properties(bioseq_samtools_hts PROPERTIES
  IMPORTED_LOCATION "${BIOSEQ_SAMTOOLS_BUILD_DIR}/htslib-1.25/libhts.a"
  INTERFACE_INCLUDE_DIRECTORIES "${BIOSEQ_SAMTOOLS_BUILD_DIR}/htslib-1.25"
  INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB;${BIOSEQ_LIBDEFLATE_TARGET};${CMAKE_DL_LIBS};m;Threads::Threads")
add_dependencies(bioseq_samtools_hts bioseq_samtools_runtime)

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
    "${BIOSEQ_IO_COMPONENT_DIR}"
    "${BIOSEQ_IO_COMPONENT_DIR}/backend/pg/include"
    "${BIOSEQ_IO_COMPONENT_DIR}/backend"
    "${BIOSEQ_IO_BENCHMARK_DIR}"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/safestringlib/include"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/fmtlog"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/fmt"
    "${BIOSEQ_IO_COMPONENT_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    reflectcpp::reflectcpp
    ZLIB::ZLIB
    ${BIOSEQ_LIBDEFLATE_TARGET}
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
    bioseq_samtools_hts
    bioseq_highway_runtime
    ${BIOSEQ_GLSLANG_STATIC_LIBS}
    bioseq_bench_framework
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BIOSEQ_IO_BENCHMARK_DIR}/main.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_IO_COMPONENT_DIR}"
    "${BIOSEQ_IO_BENCHMARK_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BIOSEQ_IO_BENCHMARK_MODULES}
  HWY 1.3.0)

add_dependencies(bioseq_io_benchmark bioseq_samtools_runtime)

target_compile_definitions(bioseq_io_benchmark
  PRIVATE
    "BIOSEQ_SAMTOOLS_EXECUTABLE=\"${BIOSEQ_SAMTOOLS_ROOT}/samtools\""
    "BIOSEQ_SAMTOOLS_BASELINE_ID=\"${BIOSEQ_SAMTOOLS_BUILD_ID}\""
    "BIOSEQ_IO_REPORT_DIR=\"${BIOSEQ_IO_COMPONENT_DIR}/../reporting/bioseq_io_benchmark\""
    "BIOSEQ_IO_DATA_DIR=\"${BIOSEQ_IO_COMPONENT_DIR}/../data\""
    "BIOSEQ_IO_BENCH_CONFIG_PATH=\"${BIOSEQ_IO_BENCHMARK_DIR}/configuration.json\"")

if(APPLE)
    target_link_options(bioseq_io_benchmark PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
