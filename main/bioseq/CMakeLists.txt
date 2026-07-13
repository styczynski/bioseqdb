# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS ${VCPKG_CMAKE_CONFIGURE_OPTIONS} -DSQLGEN_POSTGRES=1)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp)
set(BIOSEQ_GPU_ARCHIVE_BUILDER_SRC
  ${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive_builder.cpp)
set(BIOSEQ_GPU_METAL_ARCHIVE_SRC
  ${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive.cpp)
# Both builder files are compiled into bioseq_gpu_archive_builder, not the extension.
# Old probe files are excluded if still present on disk.
list(REMOVE_ITEM SRC_FILES
  ${BIOSEQ_GPU_ARCHIVE_BUILDER_SRC}
  ${BIOSEQ_GPU_METAL_ARCHIVE_SRC}
  ${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive_probe_helper.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive_probe.cpp)
file(GLOB_RECURSE SRC_MOD_FILES CONFIGURE_DEPENDS
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
find_package(ftxui REQUIRED)
find_package(simdjson REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)
find_package(TBB REQUIRED tbb)
find_package(lfreist-hwinfo REQUIRED)
find_package(HWY 1.3.0 REQUIRED NO_MODULE)
find_package(Boost 1.90.0 REQUIRED COMPONENTS program_options regex serialization iterator integer)
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_msl CONFIG REQUIRED)
find_package(VulkanUtilityLibraries CONFIG REQUIRED)

# DOC0304 Phase A — Vulkan/MoltenVK + glslang for the G3SA tiled-DP chaining
# backend. CMake's FindVulkan only materialises Vulkan::glslang when the glslang
# component is requested, so it is listed explicitly. DXC is intentionally NOT
# required: the Phase A shader is GLSL compiled through glslang, and hard-
# requiring DXC would break otherwise-valid MoltenVK / non-Windows configs.
find_package(Vulkan REQUIRED COMPONENTS glslang)

if(NOT TARGET Vulkan::glslang)
  message(FATAL_ERROR
    "vulkan-sdk-components must provide Vulkan::glslang; rerun vcpkg install vulkan-sdk-components")
endif()

set(BIOSEQ_GLSLANG_STATIC_LIBS
  ${Vulkan_glslang_LIBRARY}
  ${Vulkan_glslang-spirv_LIBRARY}
  ${Vulkan_glslang-osdependent_LIBRARY}
  ${Vulkan_glslang-machineindependent_LIBRARY}
  ${Vulkan_glslang-genericcodegen_LIBRARY})

foreach(_bioseq_glslang_lib IN LISTS BIOSEQ_GLSLANG_STATIC_LIBS)
  if(NOT _bioseq_glslang_lib OR _bioseq_glslang_lib MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
      "vulkan-sdk-components did not expose a required static glslang library; rerun vcpkg install vulkan-sdk-components")
  endif()
endforeach()

# === bench target wiring lives in main/bench/bench_v10.cmake ===
# (Old AddCommonTargetX block deleted as part of the V10 migration.
# `bioseq_bench` is now built on top of `bench_framework`.)

# ============================================================================
# DOC0269 Phase A — cross-target BMI sharing via an OBJECT library that owns
# the Domain N (NO_POSTGRES) module file set.
#
# Before this change every consumer (bioseq_aligner, bioseq_aligner_tests_exec,
# bioseq_bench, bioseq_io_bench, bioseq_bench_framework_example, …) rebuilt
# its own copy of every shared module (~68 BMIs at ~25 s each). With the
# library owning the FILE_SET CXX_MODULES PUBLIC, consumers link in the BMIs
# instead and only compile their target-private modules. See
# main/DOCS/IMPL_PLANS/BUILD_TIME_REDUCTION_VIA_SHARED_MODULE_LIBRARY_IMPORT_STD_AND_PIPE_PARTITIONS__DOC0269.md.
#
# `testing.cppm` is excluded — it pulls in Catch2 reporter headers; non-test
# binaries (bioseq_aligner CLI, bioseq_bench, bench_framework_example,
# bioseq_io_bench) don't link Catch2. Test targets add it as their own
# per-target module on top of the shared lib.
# ============================================================================

# DOC0322 Option A — Domain-N membership is now STRUCTURAL: everything under
# backend/** (and instrumentation/**) EXCEPT the Domain-P column-store set relocated
# under backend/pg/** is Domain N. Replaces the previously hand-maintained explicit
# list (and the sorted_pair_runs_pg/index_residency_pg manual appends below).
# testing.cppm stays per-target (Catch2), so it is excluded here.
file(GLOB_RECURSE BIOSEQ_NOPG_MODULES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/**/*.cppm)
list(FILTER BIOSEQ_NOPG_MODULES EXCLUDE REGEX "/backend/pg/")
list(FILTER BIOSEQ_NOPG_MODULES EXCLUDE REGEX "/common/testing\.cppm$")

# DOC0286 §12.1 Edit 1 — the IO reader (FASTA/FASTQ + gzip) + the new
# file_utils.cppm are NO_POSTGRES and are now consumed by the pg extension
# (backend/storage/io/load.cppm imports bioseq_io.public_api), so they join the
# shared nopg module set instead of the io bench's private glob. The .cppm glob
# captures the whole reader closure under io/ (including io/compression/*.cppm);
# the io bench main/tests are .cpp and excluded.
file(GLOB_RECURSE BIOSEQ_IO_MODULES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/io/*.cppm")
list(APPEND BIOSEQ_NOPG_MODULES ${BIOSEQ_IO_MODULES})
list(REMOVE_DUPLICATES BIOSEQ_NOPG_MODULES)

set(BIOSEQ_TBB_PARTITION_LINK_SHIM_SOURCE
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/tbb_partition_link_shim.cpp)
set(BIOSEQ_RUNTIME_CORE_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/hashing.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/mlogging.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/context.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/clock.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/concurrency_mode.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/concepts.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/pool.cppm)

# The external API client needs Bioseq allocation and worker-pool contracts,
# but not sequence/index/dispatch modules. Keep one canonical provider for this
# small runtime closure and share it with the full Domain-N library.
set(BIOSEQ_NOPG_OWNED_MODULES ${BIOSEQ_NOPG_MODULES})
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES ${BIOSEQ_RUNTIME_CORE_MODULES})

# DOC0372 — single-owner leaf targets extracted from the Domain-N set. These
# stay in BIOSEQ_NOPG_MODULES (so the PG glob still excludes them) but leave
# BIOSEQ_NOPG_OWNED_MODULES so they are compiled exactly once, here, with no
# runtime/TBB/logging/IO/PG edge. Isolated generated + provider metadata targets
# link these leaves without inheriting the sequence graph.
set(BIOSEQ_SINK_CONTROL_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/sink_control.cppm")
set(BIOSEQ_CHECKED_ARITHMETIC_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/checked_arithmetic.cppm")
# DOC0372 §369-374 — read_result + io_source protocol as a lightweight leaf so
# external transfer / generated clients depend on the byte-source protocol
# without the sequences/alphabets graph. The io/*.cppm glob captures it, so it
# must leave BIOSEQ_NOPG_OWNED_MODULES to be compiled exactly once in its target.
set(BIOSEQ_IO_SOURCE_CORE_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/io/source_core.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  "${BIOSEQ_SINK_CONTROL_MODULE}"
  "${BIOSEQ_CHECKED_ARITHMETIC_MODULE}"
  "${BIOSEQ_IO_SOURCE_CORE_MODULE}")

add_library(bioseq_sink_control_core STATIC)
set_target_properties(bioseq_sink_control_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_sink_control_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_sink_control_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_SINK_CONTROL_MODULE}")

add_library(bioseq_checked_arithmetic_core STATIC)
set_target_properties(bioseq_checked_arithmetic_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_checked_arithmetic_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_checked_arithmetic_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_CHECKED_ARITHMETIC_MODULE}")

add_library(bioseq_io_source_core STATIC)
set_target_properties(bioseq_io_source_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_io_source_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_io_source_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_IO_SOURCE_CORE_MODULE}")

# DOC0372 §413-424 — memory-admission controller + its sysinfo memory probe as a
# single-owner leaf, so a metadata/live target can reserve memory without
# scheduling dna4_w8. sysinfo.cppm imports nothing (POSIX/std only) and
# memory_admission.cppm now imports only the sysinfo partition (its sibling in
# this target), so the leaf carries no edge back into bioseq_modules_nopg. Both
# stay in BIOSEQ_NOPG_MODULES (PG glob still excludes them) but leave
# BIOSEQ_NOPG_OWNED_MODULES so they compile exactly once, here.
set(BIOSEQ_MEMORY_ADMISSION_CORE_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/sysinfo.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/memory_admission.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  ${BIOSEQ_MEMORY_ADMISSION_CORE_MODULES})

add_library(bioseq_memory_admission_core STATIC)
set_target_properties(bioseq_memory_admission_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_memory_admission_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_memory_admission_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_MEMORY_ADMISSION_CORE_MODULES})

# DOC0372 — lightweight templating core: value sequence, compile-time string,
# pack iterate/search, and one-time runtime dispatch. Links Highway (required by
# pack_for_each) but has no sequence, alphabet, PG, or external edge. The
# Domain-N umbrella (bioseq.common.templating) and its remaining leaves keep
# their ownership and link this core for the moved leaves' BMIs.
set(BIOSEQ_COMMON_TEMPLATING_CORE_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/value_sequence.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/template_str.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/pack_for_each.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/pack_search.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/runtime_dispatch.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  ${BIOSEQ_COMMON_TEMPLATING_CORE_MODULES})

add_library(bioseq_common_templating_core STATIC)
set_target_properties(bioseq_common_templating_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_common_templating_core PRIVATE NO_POSTGRES=1)
target_link_libraries(bioseq_common_templating_core PUBLIC hwy::hwy)
target_sources(bioseq_common_templating_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_COMMON_TEMPLATING_CORE_MODULES})

add_library(bioseq_runtime_core STATIC)
set_target_properties(bioseq_runtime_core PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_runtime_core PRIVATE NO_POSTGRES=1)
target_include_directories(bioseq_runtime_core PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt")
target_link_libraries(bioseq_runtime_core PUBLIC
  fmt
  reflectcpp::reflectcpp
  foonathan_memory
  TBB::tbb)
target_sources(bioseq_runtime_core
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_RUNTIME_CORE_MODULES})

list(REMOVE_ITEM SRC_FILES ${BIOSEQ_TBB_PARTITION_LINK_SHIM_SOURCE})

set(BIOSEQ_NOPG_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/sorting/index.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_hashmap_index.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/simd.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/vector_compare/vector_compare.cpp
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/summary_writer.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_pair_sort_sidecar.cpp)
if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  list(APPEND BIOSEQ_NOPG_SOURCES ${BIOSEQ_TBB_PARTITION_LINK_SHIM_SOURCE})
endif()
list(REMOVE_ITEM SRC_FILES ${BIOSEQ_NOPG_SOURCES})

add_library(bioseq_modules_nopg OBJECT)
set_target_properties(bioseq_modules_nopg PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  # IPO disabled here for the same reason as the existing `*_tests_exec`
  # variants (see AddPostgreSQLExtension.cmake, comment on
  # `INTERPROCEDURAL_OPTIMIZATION FALSE` for `_tests_exec`): clang's
  # ThinLTO drops module-attached TLS init wrappers (the `_ZTHW…` symbols
  # for `mlogging::fmtlogDetailT<0>::sbc`) at link time. With the lib's
  # objects feeding into every Domain N consumer — including
  # `bioseq_aligner_tests_exec` which we know reproduces the bug — the
  # lib has to stay non-LTO so the test execs link cleanly. The lib's
  # codegen is still `-O3 -DNDEBUG` per bench-opt; the only thing we
  # lose is cross-TU inlining BETWEEN the lib's objects and the
  # consumer's main TUs. Worth revisiting once the clang TLS-init bug
  # is fixed upstream (tracked in DOC0269 §12). `-O3` is preserved.
  INTERPROCEDURAL_OPTIMIZATION FALSE)

# Attach the heavy_compile Ninja pool (defined at top level when
# BIOSEQ_HEAVY_JOBS is set) so the ~70 module-interface compiles in this
# OBJECT lib don't race the same RAM as postgresql_bioseq's per-target
# module compiles. JOB_POOL_COMPILE only takes effect with the Ninja
# generator; the property is a no-op for Makefile/Xcode builds and the
# guard keeps the assignment from forcing an empty pool name when the
# user has not opted in.
if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
  set_target_properties(bioseq_modules_nopg PROPERTIES
    JOB_POOL_COMPILE heavy_compile)
endif()

# NO_POSTGRES is PRIVATE so the flag governs *this lib's own TU compilation only*
# — it does NOT propagate into linking consumers. Aligner and the test execs
# (which also need NO_POSTGRES=1) set the flag on themselves via
# AddPostgreSQLExtension.cmake's `target_compile_definitions(... PUBLIC -DNO_POSTGRES=1)`
# call for non-POSTGRES_EXTENSION targets, so the private scope here is sufficient
# to keep the existing aligner/test pipeline intact. postgresql_bioseq, which now
# also consumes this library (DOC0270), MUST NOT receive NO_POSTGRES — its
# Domain P modules include <postgres.h> and rely on palloc, ereport, etc. The
# POSTGRESQL_VERSION_MAJOR define stays PUBLIC because both Domain N and
# Domain P consumers care about the major-version-conditional code paths.
target_compile_definitions(bioseq_modules_nopg PRIVATE NO_POSTGRES=1)
target_compile_definitions(bioseq_modules_nopg PUBLIC
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})

target_include_directories(bioseq_modules_nopg PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../config"
  "${CMAKE_CURRENT_LIST_DIR}/include2"
  "${CMAKE_CURRENT_LIST_DIR}/backend"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/sorting/hwy_contrib"
  "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  ${Vulkan_INCLUDE_DIR}
  ${Boost_INCLUDE_DIRS}
  ${HWY_INCLUDE_DIRS})

if(EXISTS "/opt/homebrew/include/MoltenVK/mvk_private_api.h")
  target_include_directories(bioseq_modules_nopg PUBLIC "/opt/homebrew/include/MoltenVK")
elseif(EXISTS "/usr/local/include/MoltenVK/mvk_private_api.h")
  target_include_directories(bioseq_modules_nopg PUBLIC "/usr/local/include/MoltenVK")
endif()

target_link_libraries(bioseq_modules_nopg PUBLIC
  bioseq_runtime_core
  bioseq_sink_control_core
  bioseq_checked_arithmetic_core
  bioseq_io_source_core
  bioseq_memory_admission_core
  bioseq_common_templating_core
  safestring_static
  fmt
  ZLIB::ZLIB
  reflectcpp::reflectcpp
  libassert::assert
  foonathan_memory
  hwy::hwy
  TBB::tbb
  ${Boost_LIBRARIES})

target_link_libraries(bioseq_modules_nopg PRIVATE
  spirv-cross-msl
  Vulkan::LayerSettings)

target_sources(bioseq_modules_nopg
  PRIVATE ${BIOSEQ_NOPG_SOURCES}
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_NOPG_OWNED_MODULES})

if(APPLE)
  add_executable(bioseq_gpu_archive_builder
    ${BIOSEQ_GPU_ARCHIVE_BUILDER_SRC}
    ${BIOSEQ_GPU_METAL_ARCHIVE_SRC})
  target_compile_features(bioseq_gpu_archive_builder PRIVATE cxx_std_26)
  target_include_directories(bioseq_gpu_archive_builder PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/backend")
  if(DEFINED PostgreSQL_PKG_LIBRARY_DIR)
    install(TARGETS bioseq_gpu_archive_builder
      RUNTIME DESTINATION "${PostgreSQL_PKG_LIBRARY_DIR}"
      COMPONENT ${PROJECT_NAME})
  endif()
endif()

# ============================================================================
# Tests/integration-tests module file lists. testing.cppm is per-target
# because the shared lib excludes Catch2.
# ============================================================================

file(GLOB_RECURSE A_SRC_FILES
  ${CMAKE_CURRENT_LIST_DIR}/cli/cli_main.cpp)
file(GLOB_RECURSE A_SRC_TESTS
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/testing.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/*_test.cpp
  # DOC0286 §13 — NO_POSTGRES file_utils unit test. Listed explicitly (NOT the
  # whole io/*_test.cpp glob) so the heavyweight io_test.cpp (kseq++) stays out
  # of the unit exec; file_utils only needs bioseq_io.{file_utils,options}, now
  # in bioseq_modules_nopg.
  ${CMAKE_CURRENT_LIST_DIR}/io/file_utils_test.cpp
  # DOC0295 §13.7 — NO_POSTGRES identifier-parser unit tests (formats/tables,
  # lexer, resolver order, FASTQ sampler). Same lightweight pattern.
  ${CMAKE_CURRENT_LIST_DIR}/io/identifier_parser/identifier_formats_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/io/identifier_parser/identifier_parser_test.cpp)

file(GLOB_RECURSE A_INTEGRATION_SRC_TESTS
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/testing.cppm
  ${CMAKE_CURRENT_LIST_DIR}/integration_tests/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/integration_tests/*.cpp)

# Integration-tests module set: shared lib provides mlogging.cppm + the
# whole Domain N core, so we only list testing.cppm + the integration-test-
# specific modules here.
file(GLOB_RECURSE A_INTEGRATION_SRC_TESTS_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/testing.cppm
  ${CMAKE_CURRENT_LIST_DIR}/integration_tests/*.cppm)

list (APPEND A_SRC_TESTS ${A_SRC_FILES})

list(REMOVE_ITEM SRC_FILES ${A_SRC_TESTS})
list(REMOVE_ITEM SRC_MOD_FILES ${A_SRC_TESTS})

# DOC0270 — drop every module that the shared bioseq_modules_nopg OBJECT
# library already owns from postgresql_bioseq's per-target module set. Without
# this `list(REMOVE_ITEM)`, postgresql_bioseq's `SRC_MOD_FILES` GLOB collects
# all backend/**/*.cppm files (including the ~50 Domain N modules already in
# the shared lib) and ninja rebuilds them inside the extension's own target —
# the same ~25 s × ~50 BMI compile that DOC0269 Phase A eliminated for the
# aligner / tests but never reached the postgres extension. The
# `SHARED_MODULE_LIBS bioseq_modules_nopg` argument in the
# `AddCommonTargetX(postgresql_bioseq …)` call below links the lib's BMIs in
# instead. Net effect: the Domain N module set is compiled exactly once
# across the whole build, not twice (lib + postgresql_bioseq) as before.
#
# IMPORTANT — the lib's BMIs are baked with NO_POSTGRES=1 (lib-private), so
# the two Domain N modules that branch on NO_POSTGRES end up with the
# DEBUG_ASSERT variant in postgresql_bioseq:
#
#   - sequences/simd.cppm  ::stdc_compare contract-violation paths use
#     DEBUG_ASSERT instead of PostgreSQL ERROR logging. With -DNDEBUG the
#     DEBUG_ASSERT compiles out, so the violations become silent. These
#     paths are unreachable in well-formed code (caller passes mixed-alphabet
#     stdc values, or cross-storage-width non-bit-castable pair); the
#     follow-up to keep the loud PostgreSQL ERROR behaviour is to extract
#     those calls into a Domain P wrapper that postgresql_bioseq imports
#     separately — not done here to keep the diff scoped to the build-time fix.
#   - common/mlogging.cppm remains PG-header-free. PG-target logging is
#     delivered through the sink registered by _PG_init(); before registration
#     or under NO_POSTGRES, it falls back to BIOSEQ_LOG_FILE or
#     /tmp/bioseq_loglog.log.
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_NOPG_MODULES})
list(REMOVE_DUPLICATES SRC_MOD_FILES)

# DOC0372 — the external CustomScan (backend/pg/external/scan.cppm) and its row
# writer (row_writer.cppm) import the external provider stack, so they compile
# into the extension only when the external API clients are enabled. The rest of
# backend/pg/external/ (relation/errors/path_builder_slot) imports no provider
# module and stays unconditional. The option is declared here (before
# SRC_MOD_FILES is consumed by the postgresql_bioseq target); the later
# add_subdirectory(external) reuses the same cached value.
option(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
  "Configure generated external API clients and their integration tests" OFF)
if(NOT BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  list(FILTER SRC_MOD_FILES EXCLUDE
    REGEX "/backend/pg/external/(scan|row_writer)\\.cppm$")
else()
  # DOC0372 streaming data plane — the external CustomScan's producer task
  # (scan.cppm) imports the libcurl pull source, locator SSRF firewall, serial
  # gzip decode source, and NCBI sequence stream. Compile those non-PG transfer/IO
  # modules directly into the extension (like scan/row_writer) so there is one
  # copy of the shared Domain-N module objects (nopg arrives via SHARED_MODULE_LIBS)
  # rather than a second copy through a static library.
  list(APPEND SRC_MOD_FILES
    "${CMAKE_CURRENT_LIST_DIR}/external/transport/locator.cppm"
    "${CMAKE_CURRENT_LIST_DIR}/external/transport/remote_source.cppm"
    "${CMAKE_CURRENT_LIST_DIR}/external/io/gzip_decode_source.cppm"
    "${CMAKE_CURRENT_LIST_DIR}/external/io/sequence_stream.cppm"
    "${CMAKE_CURRENT_LIST_DIR}/external/io/archive_source.cppm"
    "${CMAKE_CURRENT_LIST_DIR}/external/io/datasets_stream.cppm")
  # archive_md5.cpp is a classic (non-module) TU: it owns the OpenSSL include so
  # <openssl/evp.h> never enters a module global module fragment.
  list(APPEND SRC_FILES
    "${CMAKE_CURRENT_LIST_DIR}/external/io/archive_md5.cpp")
endif()

# GCC 16.1 can spend many hours and >64 GiB RSS compiling one typed
# dispatch wrapper at -O3. Keep this as a build knob, not a C++ ifdef:
# runtime dispatch and algorithm selection are unchanged, and non-GCC
# builds stay on the preset's normal -O3 path.
set(BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL "O1" CACHE STRING
  "Optimization level for GCC 16.1 AWS typed dispatch wrapper TUs. \
Allowed: O0, O1, O2, O3. O3 preserves the global preset behavior.")
set_property(CACHE BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL PROPERTY STRINGS O0 O1 O2 O3)
if(NOT BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL MATCHES "^O[0123]$")
  message(FATAL_ERROR
    "BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL must be one of O0, O1, O2, O3; got "
    "'${BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL}'")
endif()

set(BIOSEQ_GCC_TYPED_DISPATCH_JOBS "1" CACHE STRING
  "Ninja compile-pool width for GCC 16.1 AWS typed dispatch wrapper TUs. \
0 = inherit the target compile pool.")
if(NOT BIOSEQ_GCC_TYPED_DISPATCH_JOBS MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "BIOSEQ_GCC_TYPED_DISPATCH_JOBS must be a non-negative integer; got "
    "'${BIOSEQ_GCC_TYPED_DISPATCH_JOBS}'")
endif()

set(BIOSEQ_GCC_TEMPLATE_FORCE_INLINE "ON" CACHE BOOL
  "Keep forced always_inline/flatten attributes in template dispatch modules \
for the GCC 16.1 AWS preset. OFF preserves algorithm selection but lets GCC \
avoid constructing very large inlining graphs while compiling typed dispatch.")

set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w1_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w2_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w4_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna4_w8_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w1_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w2_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w4_inmemory_chunked_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8_scanner_pg.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8_inmemory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8_inmemory_standard.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8_inmemory_segmented.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8_inmemory_chunked.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl_dna5_w8_inmemory_chunked_segmented.cpp)
if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  if(NOT BIOSEQ_GCC_TEMPLATE_FORCE_INLINE)
    target_compile_definitions(bioseq_modules_nopg PRIVATE
      BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE=0
      BIOSEQ_PIPELINE_FORCE_INLINE=0)
  endif()
  if(BIOSEQ_GCC_TYPED_DISPATCH_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
      gcc_typed_dispatch_compile=${BIOSEQ_GCC_TYPED_DISPATCH_JOBS})
    set_source_files_properties(${BIOSEQ_DISPATCH_PROCESSOR_TYPED_SOURCES}
      PROPERTIES JOB_POOL_COMPILE gcc_typed_dispatch_compile)
  endif()
  if(NOT BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL STREQUAL "O3")
    set_source_files_properties(${BIOSEQ_DISPATCH_PROCESSOR_TYPED_SOURCES}
      PROPERTIES COMPILE_OPTIONS "-${BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL}")
  endif()
  message(STATUS
    "bioseq: GCC typed dispatch wrapper TUs use "
    "-${BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL}, compile jobs="
    "${BIOSEQ_GCC_TYPED_DISPATCH_JOBS}, template force-inline="
    "${BIOSEQ_GCC_TEMPLATE_FORCE_INLINE}; set "
    "BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL=O3 and "
    "BIOSEQ_GCC_TEMPLATE_FORCE_INLINE=ON to restore the global preset")
endif()

# Per-target module set for the aligner (and its tests_exec sibling): just
# testing.cppm; every other Domain N module is delivered via
# bioseq_modules_nopg. tests_exec adds the _test.cpp files to SOURCES via
# AddCommonTargetX's TESTS arg.
set(A_ALIGNER_MOD_FILES
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/testing.cppm)

AddCommonTargetX(
  bioseq_aligner
  TYPE EXEC
  MAIN "cli_main\.cpp"
  CXX 26
  RELOCATABLE
  NAME bioseq_aligner
  COMMENT "BioSeq CLI"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../config"
    "${CMAKE_CURRENT_LIST_DIR}/include2"
    "${CMAKE_CURRENT_LIST_DIR}/backend"
    "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  TESTS
    ${A_SRC_TESTS}
  INTEGRATION_TESTS
    ${A_INTEGRATION_SRC_TESTS}
  INTEGRATION_TESTS_LINKS
    sqlgen::sqlgen
  INTEGRATION_TESTS_MODULES
    ${A_INTEGRATION_SRC_TESTS_MODULES}
  LINKS
    safestring_static
    fmt
    ZLIB::ZLIB
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    TBB::tbb
    Vulkan::Vulkan
    Vulkan::glslang
    spirv-cross-msl
    Vulkan::LayerSettings
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    ${A_SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
  CXX_MOD_FILES
    ${A_ALIGNER_MOD_FILES}
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql")

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
    TBB::tbb
    ${BIOSEQ_GLSLANG_STATIC_LIBS}
    spirv-cross-msl
    Vulkan::LayerSettings
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
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

if(TARGET bioseq_gpu_archive_builder)
  add_dependencies(postgresql_bioseq bioseq_gpu_archive_builder)
endif()

# DOC0270 — disable LTO on the Postgres extension. The default IPO=TRUE set
# by AddPostgreSQLExtension.cmake doubles the per-build wall-clock for the
# heavy dispatch_buffer.cppm template fan-out: clang first instantiates the
# full (alphabet × storage_unit_byte_width) Cartesian product at per-TU
# compile time, then ThinLTO re-runs inlining/optimisation across all TUs at
# link time. The pre-DOC0270 baseline (DOC0268 §2) timed out a fresh
# postgresql_bioseq build at the 50 min hard ceiling; the link-phase
# template-instantiation tail accounts for a sizeable share of that. The
# bioseq_modules_nopg OBJECT library also runs with IPO=FALSE (see the
# comment on its property block above) for the same TLS-init-wrapper bug
# that drove the test execs to opt out; keeping the postgresql_bioseq
# linkage non-LTO matches that posture.
set_target_properties(postgresql_bioseq PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE
  # DOC0341 — the PostgreSQL extension must NOT link-load libvulkan in the
  # postmaster, because the macOS loader stack maps Metal/IOAccelerator before
  # backend fork. Keep the vcpkg rpath only so Runtime::init() can later
  # dlopen("@rpath/libvulkan...") inside the selected-GPU backend process.
  INSTALL_RPATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib;${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib")

# Throttle the per-target heavy module compiles on low-RAM hosts. The
# postgresql_bioseq target owns the Domain P module fan-out — the modules
# that import the columnar storage / metadata / table-io stack and feel
# the most BMI-load + template-instantiation pressure (utils.cppm,
# access_method.cppm, columnar_customscan.cppm, columnar_aggregator_node
# .cppm, …). When BIOSEQ_HEAVY_JOBS is set the assignment caps these
# compiles against the same Ninja pool used by bioseq_modules_nopg, so
# the worst-case concurrent peak RSS is `heavy_compile × per-TU peak`
# instead of `min(--parallel, ncpu) × per-TU peak`. Same Ninja-generator
# caveat as the lib's annotation above; pool is unset (no-op) on other
# generators or when the user has not opted in.
if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
  set_target_properties(postgresql_bioseq PROPERTIES
    JOB_POOL_COMPILE heavy_compile)
  if(TARGET bioseq_aligner)
    set_target_properties(bioseq_aligner PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
  if(TARGET bioseq_aligner_tests_exec)
    set_target_properties(bioseq_aligner_tests_exec PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
  if(TARGET bioseq_aligner_int_tests_exec)
    set_target_properties(bioseq_aligner_int_tests_exec PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
endif()

# DOC0279 §11.5/§16.2 — compile the test-only indexing descriptors
# (TestTableCountIndex, a CHUNKED_PER_TABLE index) into the server-loaded
# extension dylib AND the integration/unit test execs. Default OFF (production);
# the integration CI preset sets -DBIOSEQ_TEST_INDEXES=ON so the
# [indexing][per-table] test and the corruption-hook tests run.
option(BIOSEQ_TEST_INDEXES
  "Compile the test-only indexing descriptors into the extension + test execs" OFF)
if(BIOSEQ_TEST_INDEXES)
  target_compile_definitions(postgresql_bioseq PRIVATE BIOSEQ_TEST_INDEXES=1)
  if(TARGET bioseq_aligner_int_tests_exec)
    target_compile_definitions(bioseq_aligner_int_tests_exec PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
  if(TARGET bioseq_aligner_tests_exec)
    target_compile_definitions(bioseq_aligner_tests_exec PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/io/build.cmake")

# bench_framework — generic, algorithm-agnostic benchmark orchestration.
include("${CMAKE_CURRENT_LIST_DIR}/../bench_framework/build.cmake")

# V10 aligner bench target (bioseq_bench), built on top of bench_framework.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/bench_v10.cmake")

# DOC0286 Part 2 — end-to-end SQL bench target (bioseq_bench_e2e), drives
# postgres over libpq (bioseq.load + mmsearch) vs minimap2.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/bench_e2e.cmake")

# External API descriptors are build-time inputs. Keep their generated clients
# outside BIOSEQ_NOPG_MODULES so changing an OpenAPI schema does not rebuild the
# shared Domain-N module library. The dedicated build mode enables this option;
# normal configurations must not require the host's code-generation toolchain.
option(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
  "Configure generated external API clients and their integration tests" OFF)
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  set(BIOSEQ_EXTERNAL_GENERATED_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/external")
  add_subdirectory(external)

  # DOC0372 — link the compile-time provider pack into the extension so the
  # external CustomScan (scan.cppm) and row writer resolve bioseq.external.run
  # and the provider model, and expose those module BMIs to the extension's
  # module compiles. BIOSEQ_EXTERNAL_PG gates the _PG_init registration and the
  # conditional import of the scan module in backend/pg/column/column.cppm.
  if(TARGET bioseq_external_provider_adapters)
    # DOC0372 streaming data plane transfer/archive dependencies (see the
    # SRC_MOD_FILES append above for the transport/IO modules compiled in).
    find_package(CURL REQUIRED)
    find_package(c-ares CONFIG REQUIRED)
    find_package(LibArchive REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(postgresql_bioseq PRIVATE
      bioseq_external_provider_core
      bioseq_external_provider_adapters
      CURL::libcurl
      c-ares::cares
      LibArchive::LibArchive
      OpenSSL::Crypto
      ZLIB::ZLIB)
    target_compile_definitions(postgresql_bioseq PRIVATE BIOSEQ_EXTERNAL_PG=1)
  endif()
endif()
