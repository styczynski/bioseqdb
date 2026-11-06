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
set(BIOSEQ_SAMTOOLS_BUILD_DIR "${CMAKE_BINARY_DIR}/bench/tool_samtools")
ExternalProject_Add(bioseq_samtools_baseline
  SOURCE_DIR "${BIOSEQ_SAMTOOLS_BUILD_DIR}"
  DOWNLOAD_COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${PROJECT_SOURCE_DIR}/bench/tool_samtools" "${BIOSEQ_SAMTOOLS_BUILD_DIR}"
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
    "CC=${CMAKE_C_COMPILER}" "CFLAGS=-O3 -DNDEBUG"
    "CPPFLAGS=-I${ZLIB_INCLUDE_DIRS}"
    "LDFLAGS=-L${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
    <SOURCE_DIR>/configure --without-curses --disable-bz2 --disable-lzma
      --disable-libcurl --with-libdeflate --enable-configure-htslib --with-htslib=htslib-1.25
  BUILD_COMMAND "${BIOSEQ_SAMTOOLS_MAKE}" -j2 samtools
  BUILD_IN_SOURCE TRUE
  BUILD_BYPRODUCTS "${BIOSEQ_SAMTOOLS_BUILD_DIR}/samtools"
    "${BIOSEQ_SAMTOOLS_BUILD_DIR}/htslib-1.25/libhts.a"
  INSTALL_COMMAND ""
  EXCLUDE_FROM_ALL TRUE)
add_library(bioseq_samtools_hts STATIC IMPORTED)
set_target_properties(bioseq_samtools_hts PROPERTIES
  IMPORTED_LOCATION "${BIOSEQ_SAMTOOLS_BUILD_DIR}/htslib-1.25/libhts.a"
  INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/bench/tool_samtools/htslib-1.25"
  INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB;${BIOSEQ_LIBDEFLATE_TARGET};${CMAKE_DL_LIBS};m;Threads::Threads")
add_dependencies(bioseq_samtools_hts bioseq_samtools_baseline)

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

target_compile_definitions(bioseq_io_benchmark
  PRIVATE
    "BIOSEQ_SAMTOOLS_EXECUTABLE=\"${BIOSEQ_SAMTOOLS_BUILD_DIR}/samtools\""
    "BIOSEQ_IO_REPORT_DIR=\"${BIOSEQ_IO_COMPONENT_DIR}/../reporting/bioseq_io_benchmark\""
    "BIOSEQ_IO_DATA_DIR=\"${BIOSEQ_IO_COMPONENT_DIR}/../data\""
    "BIOSEQ_IO_BENCH_CONFIG_PATH=\"${BIOSEQ_IO_BENCHMARK_DIR}/configuration.json\"")

if(APPLE)
    target_link_options(bioseq_io_benchmark PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
