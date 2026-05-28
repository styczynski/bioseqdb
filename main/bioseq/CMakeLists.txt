# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS ${VCPKG_CMAKE_CONFIGURE_OPTIONS} -DSQLGEN_POSTGRES=1)

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
find_package(ftxui REQUIRED)
find_package(simdjson REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)
find_package(TBB REQUIRED tbb)
find_package(lfreist-hwinfo REQUIRED)
find_package(HWY 1.3.0 REQUIRED NO_MODULE)
find_package(Boost 1.90.0 REQUIRED COMPONENTS program_options regex serialization iterator integer)

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

set(BIOSEQ_NOPG_MODULES
  # backend/alignment_hwy/* (Highway-bound SIMD aligners)
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/lane_helpers.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/utils.cppm
  # backend/common/*  (testing.cppm excluded — Catch2 dependency)
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/algo_tracer.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/circular_append_buffer.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/fragment_cache.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/context.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/mlogging.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/sequan_compat.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/utils.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/vector_stream.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/sorting/radix_sort.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/dispatch_chain.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/pack_for_each.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/pack_search.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/runtime_dispatch.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/template_str.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/templating.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/value_sequence.cppm
  # backend/config/config.cppm is Domain P (imports pg_utils, uses PG macros);
  # it stays in postgresql_bioseq's own module set.
  # backend/storage/* — the PG-free bucket math + seq_locator POD
  # (DOC0260 §12.1) plus, since DOC0263 §2.2, the standalone sequence_size
  # module and the new lazy-fragmented-sequence machinery (scanner concept +
  # lazy_compact_sequence wrapper). All are NO_POSTGRES-safe.
  # sequence_storage_manager.cppm stays in postgresql_bioseq's own set (it uses
  # the PG catalog / palloc).
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/bucket_math.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/sequence_size.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/sequence_scanner.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_compact_sequence.cppm
  # backend/sequences/*
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/cigar.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/index.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/sequences.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/simd.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_rmq.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_scalar.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_utils.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/index.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/mm_sketch_baseline.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/base.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/index.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/lexic.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/minimap_int.cppm
  # DOC0264 §10.1 — sample-sequence generator moved from main/bench/
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/generation/sequence_generator.cppm
  # backend/pipeline/*
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe.cppm
  # instrumentation/*
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/access.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/aggregate.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/arena.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/clock.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/config.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/counter.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/descriptor.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/arch.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/do_not_optimize.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/siglock.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/spinlock.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/summary_writer.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/sysinfo.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/gauge.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/global_state.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/histogram.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/loop.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/mod.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/pmu.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/pmu_linux.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/pmu_macos.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/pmu_stub.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/pool.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/region.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/sample.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/sampler/sampler.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/sampler/sampler_posix.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/sampler/sampler_stub.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/thread_state.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/trace.cppm
)

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

# NO_POSTGRES is PRIVATE so the flag governs *this lib's own TU compilation only*
# — it does NOT propagate into linking consumers. Aligner and the test execs
# (which also need NO_POSTGRES=1) set the flag on themselves via
# AddPostgreSQLExtension.cmake's `target_compile_definitions(... PUBLIC -DNO_POSTGRES=1)`
# call for non-POSTGRES_EXTENSION targets, so the private scope here is sufficient
# to keep the existing aligner/test pipeline intact. postgresql_bioseq, which now
# also consumes this library (DOC0270), MUST NOT receive NO_POSTGRES — its
# Domain P modules include <postgres.h> and rely on pg_log, palloc, etc. The
# POSTGRESQL_VERSION_MAJOR define stays PUBLIC because both Domain N and
# Domain P consumers care about the major-version-conditional code paths.
target_compile_definitions(bioseq_modules_nopg PRIVATE NO_POSTGRES=1)
target_compile_definitions(bioseq_modules_nopg PUBLIC
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})

target_include_directories(bioseq_modules_nopg PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../config"
  "${CMAKE_CURRENT_LIST_DIR}/include2"
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/backend"
  "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  ${Boost_INCLUDE_DIRS}
  ${HWY_INCLUDE_DIRS})

target_link_libraries(bioseq_modules_nopg PUBLIC
  safestring_static
  fmt
  ZLIB::ZLIB
  reflectcpp::reflectcpp
  libassert::assert
  foonathan_memory
  hwy::hwy
  ${Boost_LIBRARIES})

target_sources(bioseq_modules_nopg
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_NOPG_MODULES})

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
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/*_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/*_test.cpp)

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
#     DEBUG_ASSERT instead of pg_log(ERROR, …). With -DNDEBUG the
#     DEBUG_ASSERT compiles out, so the violations become silent. These
#     paths are unreachable in well-formed code (caller passes mixed-alphabet
#     stdc values, or cross-storage-width non-bit-castable pair); the
#     follow-up to keep the loud-pg_log behaviour is to extract those calls
#     into a Domain P wrapper that postgresql_bioseq imports separately —
#     not done here to keep the diff scoped to the build-time fix.
#   - common/mlogging.cppm ::log_configure() skips the postgres-specific
#     fmtlog::setLogFile branch; logs route to stderr (Postgres server log)
#     instead of /tmp/bioseq_loglog.log. Observable but not a correctness
#     regression — the operator can re-route via BIOSEQ_LOG_FILE if needed.
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_NOPG_MODULES})

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
    "${CMAKE_CURRENT_LIST_DIR}/include"
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
  INTERPROCEDURAL_OPTIMIZATION FALSE)

include("${CMAKE_CURRENT_LIST_DIR}/io/build.cmake")

# bench_framework — generic, algorithm-agnostic benchmark orchestration.
include("${CMAKE_CURRENT_LIST_DIR}/../bench_framework/build.cmake")

# V10 aligner bench target (bioseq_bench), built on top of bench_framework.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/bench_v10.cmake")
