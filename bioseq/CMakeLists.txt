# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS ${VCPKG_CMAKE_CONFIGURE_OPTIONS} -DSQLGEN_POSTGRES=1)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_source_inventory.cmake")
option(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST
  "Run the maintainer PREPARE incremental Ninja contract during the build" ON)
option(BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST
  "Run the maintainer SQL negative/provenance fixture suite during the build" ON)
option(BIOSEQ_TEST_INDEXES
  "Compile the test-only indexing descriptors into the extension + test execs"
  OFF)
set(BIOSEQ_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS "50" CACHE STRING
  "Retry interval in milliseconds for a conditional vacuum truncate lock")
set(BIOSEQ_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS "4500" CACHE STRING
  "Total timeout in milliseconds for a conditional vacuum truncate lock")
foreach(_bioseq_vacuum_lock_value IN ITEMS
    BIOSEQ_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS
    BIOSEQ_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS)
  if(NOT "${${_bioseq_vacuum_lock_value}}" MATCHES "^[1-9][0-9]*$"
      OR ${${_bioseq_vacuum_lock_value}} GREATER 2147483647)
    message(FATAL_ERROR
      "${_bioseq_vacuum_lock_value} must be an integer from 1 through 2147483647")
  endif()
endforeach()
if(BIOSEQ_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS GREATER
    BIOSEQ_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS)
  message(FATAL_ERROR
    "BIOSEQ_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS cannot exceed "
    "BIOSEQ_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS")
endif()
set(_bioseq_build_config_authored_input
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/bioseq_build_config.cppm")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_bioseq_build_config_authored_input}")
_bioseq_build_config_template_flatten_default(
  "${_bioseq_build_config_authored_input}"
  _bioseq_default_template_flatten_profile_ordinal
  BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE)
_bioseq_build_config_profiles(BIOSEQ_TEMPLATE_FLATTEN_PROFILES)

# Keep the authored build-config declaration as the everyday source of truth while
# allowing isolated qualification presets to compile the complete profile in
# a separate build tree. An empty cache entry selects the authored default;
# named qualification presets set the same public profile key explicitly.
set(BIOSEQ_TEMPLATE_FLATTEN_PROFILE "" CACHE STRING
  "Template-flatten specialization profile (empty selects authored default)")
set_property(CACHE BIOSEQ_TEMPLATE_FLATTEN_PROFILE PROPERTY
  STRINGS "" ${BIOSEQ_TEMPLATE_FLATTEN_PROFILES})
if("${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}" STREQUAL "")
  set(BIOSEQ_TEMPLATE_FLATTEN_PROFILE
    "${BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE}")
  set(_bioseq_template_flatten_profile_source "authored build-config default")
else()
  list(FIND BIOSEQ_TEMPLATE_FLATTEN_PROFILES
    "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}"
    _bioseq_template_flatten_profile_index)
  if(_bioseq_template_flatten_profile_index EQUAL -1)
    message(FATAL_ERROR
      "unsupported BIOSEQ_TEMPLATE_FLATTEN_PROFILE "
      "'${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}'")
  endif()
  set(_bioseq_template_flatten_profile_source "explicit cache selection")
endif()

_bioseq_build_config_profile(
  "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}"
  _bioseq_template_flatten_profile_ordinal
  BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64
  BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES
  BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS
  BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES
  BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION
  _bioseq_template_flatten_scheduler_unit_count)
set(_bioseq_template_flatten_capability_definitions)

foreach(_bioseq_template_flatten_capability IN ITEMS
    BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64
    BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES
    BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS
    BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES
    BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION)
  if(${_bioseq_template_flatten_capability})
    list(APPEND _bioseq_template_flatten_capability_definitions
      "${_bioseq_template_flatten_capability}=1")
  endif()
endforeach()
# Publish the resolved semantic profile and cumulative capabilities to every
# translation unit so generated partitions, authored sources, and the owner
# manifest cannot disagree.
add_compile_definitions(
  BIOSEQ_TEMPLATE_FLATTEN_PROFILE=${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}
  ${_bioseq_template_flatten_capability_definitions})
message(STATUS
  "bioseq: BIOSEQ_TEMPLATE_FLATTEN_PROFILE="
  "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE} "
  "(${_bioseq_template_flatten_profile_source}; authored default="
  "${BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE}, ordinal "
  "${_bioseq_default_template_flatten_profile_ordinal})")

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp)
set(BIOSEQ_GPU_ARCHIVE_MODULE_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/builder.cppm")
set(BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/command_capture.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/main.cppm")

file(GLOB_RECURSE SRC_MOD_FILES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/**/*.cppm)
# The authored interface is generator input only. The generated target below
# is the sole compiled provider of bioseq.build_config.
list(REMOVE_ITEM SRC_MOD_FILES
  "${_bioseq_build_config_authored_input}")
_bioseq_build_config_bool(
  "${BIOSEQ_TEST_INDEXES}" _bioseq_build_config_test_indexes)
_bioseq_build_config_bool(
  "${BIOSEQ_BUILD_EXTERNAL_API_CLIENTS}"
  _bioseq_build_config_external_api_clients)
_bioseq_build_config_cache_testing(
  "${BIOSEQ_BUILD_CACHE}" "${BIOSEQ_CACHE_TESTING}"
  _bioseq_build_config_cache_testing)
bioseq_add_build_config_module(
  TARGET bioseq_build_config
  SOURCE "${_bioseq_build_config_authored_input}"
  OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated/build_config"
  PROFILE "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}"
  TEST_INDEXES "${_bioseq_build_config_test_indexes}"
  CACHE_TESTING "${_bioseq_build_config_cache_testing}"
  EXTERNAL_API_CLIENTS "${_bioseq_build_config_external_api_clients}"
  VACUUM_WAIT_MILLISECONDS
    "${BIOSEQ_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS}"
  VACUUM_TIMEOUT_MILLISECONDS
    "${BIOSEQ_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS}"
  OUT_MODULE BIOSEQ_BUILD_CONFIG_GENERATED_MODULE
  OUT_COMPILE_CONTRACT BIOSEQ_BUILD_CONFIG_GENERATED_COMPILE_CONTRACT
  OUT_MANIFEST BIOSEQ_BUILD_CONFIG_GENERATED_MANIFEST
  OUT_STAMP BIOSEQ_BUILD_CONFIG_GENERATED_STAMP
  OUT_SCHEDULER_UNIT_COUNT BIOSEQ_BUILD_CONFIG_SCHEDULER_UNIT_COUNT)
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_GPU_ARCHIVE_MODULE_UNITS}
  ${BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS})

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_source_guards.cmake")

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

set(_bioseq_configured_sanitizer_flags
  "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} "
  "${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS}")
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _bioseq_build_type_upper)
  foreach(_bioseq_flag_kind IN ITEMS
      C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER)
    set(_bioseq_config_flag_var
      "CMAKE_${_bioseq_flag_kind}_FLAGS_${_bioseq_build_type_upper}")
    string(APPEND _bioseq_configured_sanitizer_flags
      " ${${_bioseq_config_flag_var}}")
  endforeach()
endif()
if(_bioseq_configured_sanitizer_flags MATCHES
    "(^|[ ;])-fsanitize=([^ ;]*,)?thread(,[^ ;]*)?($|[ ;])")
  set(_bioseq_tbb_tsan_marker
    "${TBB_DIR}/bioseq-tsan-instrumented.txt")
  if(NOT EXISTS "${_bioseq_tbb_tsan_marker}")
    message(FATAL_ERROR
      "ThreadSanitizer builds require a TSan-instrumented oneTBB runtime; "
      "missing ${_bioseq_tbb_tsan_marker}. Configure with the "
      "test-opt-tsan preset so its vcpkg overlay rebuilds oneTBB with "
      "TBB_SANITIZE=thread.")
  endif()
  file(READ "${_bioseq_tbb_tsan_marker}" _bioseq_tbb_tsan_marker_value)
  string(STRIP "${_bioseq_tbb_tsan_marker_value}"
    _bioseq_tbb_tsan_marker_value)
  string(CONCAT _bioseq_tbb_tsan_expected_marker
    "TBB_SANITIZE=thread\n"
    "CMAKE_C_COMPILER=/opt/homebrew/opt/llvm@22/bin/clang\n"
    "CMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@22/bin/clang++")
  if(NOT _bioseq_tbb_tsan_marker_value STREQUAL
      _bioseq_tbb_tsan_expected_marker)
    message(FATAL_ERROR
      "ThreadSanitizer oneTBB marker has unexpected content: "
      "${_bioseq_tbb_tsan_marker}")
  endif()
endif()
unset(_bioseq_tbb_tsan_expected_marker)
unset(_bioseq_tbb_tsan_marker_value)
unset(_bioseq_tbb_tsan_marker)
unset(_bioseq_config_flag_var)
unset(_bioseq_flag_kind)
unset(_bioseq_build_type_upper)
unset(_bioseq_configured_sanitizer_flags)

find_package(lfreist-hwinfo REQUIRED)
find_package(HWY 1.3.0 REQUIRED NO_MODULE)
find_package(Boost 1.90.0 REQUIRED COMPONENTS program_options regex serialization iterator integer)
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_msl CONFIG REQUIRED)
find_package(VulkanUtilityLibraries CONFIG REQUIRED)

# Vulkan/MoltenVK and glslang provide the G3SA tiled-DP chaining backend.
# Discover glslang through its package config rather than FindVulkan's legacy
# component shim.  Current glslang packages no longer install OGLCompiler, which
# older FindVulkan modules require before they create Vulkan::glslang.  Linking
# the package's exported targets works with both the pinned Apple and Linux
# toolchains and does not change the compiled shader or pipeline semantics.
if(APPLE AND BIOSEQ_BENCHMARK_E2E_INTERVALS)
  if(NOT DEFINED VCPKG_INSTALLED_DIR
      OR NOT IS_ABSOLUTE "${VCPKG_INSTALLED_DIR}"
      OR NOT DEFINED VCPKG_TARGET_TRIPLET
      OR "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    message(FATAL_ERROR
      "Optimized Apple E2E receipts require one absolute vcpkg installation root")
  endif()
  set(_bioseq_e2e_vulkan_release_candidate
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libvulkan.dylib")
  if(NOT EXISTS "${_bioseq_e2e_vulkan_release_candidate}")
    message(FATAL_ERROR
      "Optimized Apple E2E Vulkan release loader is absent: "
      "${_bioseq_e2e_vulkan_release_candidate}")
  endif()
  file(REAL_PATH "${_bioseq_e2e_vulkan_release_candidate}"
    _bioseq_e2e_vulkan_release_loader)
  file(REAL_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
    _bioseq_e2e_vulkan_receipt_root)
  cmake_path(IS_PREFIX _bioseq_e2e_vulkan_receipt_root
    "${_bioseq_e2e_vulkan_release_loader}" NORMALIZE
    _bioseq_e2e_vulkan_release_is_receipt_owned)
  string(FIND "${_bioseq_e2e_vulkan_release_loader}" "/debug/lib/"
    _bioseq_e2e_vulkan_debug_offset)
  if(NOT _bioseq_e2e_vulkan_release_is_receipt_owned
      OR NOT _bioseq_e2e_vulkan_debug_offset EQUAL -1)
    message(FATAL_ERROR
      "Optimized Apple E2E Vulkan loader is not the receipt-owned release image: "
      "${_bioseq_e2e_vulkan_release_loader}")
  endif()
  set(Vulkan_LIBRARY "${_bioseq_e2e_vulkan_release_loader}"
    CACHE FILEPATH
    "Receipt-owned release Vulkan loader for optimized Apple E2E builds"
    FORCE)
endif()
find_package(Vulkan REQUIRED)
find_package(glslang CONFIG REQUIRED)

if(APPLE AND BIOSEQ_BENCHMARK_E2E_INTERVALS)
  if(NOT TARGET Vulkan::Vulkan)
    message(FATAL_ERROR
      "FindVulkan did not create Vulkan::Vulkan for optimized Apple E2E")
  endif()
  get_target_property(_bioseq_e2e_vulkan_imported_loader
    Vulkan::Vulkan IMPORTED_LOCATION)
  if(NOT _bioseq_e2e_vulkan_imported_loader)
    message(FATAL_ERROR
      "Vulkan::Vulkan has no imported location for optimized Apple E2E")
  endif()
  file(REAL_PATH "${Vulkan_LIBRARY}"
    _bioseq_e2e_vulkan_selected_loader)
  file(REAL_PATH "${_bioseq_e2e_vulkan_imported_loader}"
    _bioseq_e2e_vulkan_imported_loader)
  string(FIND "${_bioseq_e2e_vulkan_selected_loader}" "/debug/lib/"
    _bioseq_e2e_vulkan_selected_debug_offset)
  string(FIND "${_bioseq_e2e_vulkan_imported_loader}" "/debug/lib/"
    _bioseq_e2e_vulkan_imported_debug_offset)
  if(NOT "${_bioseq_e2e_vulkan_selected_loader}" STREQUAL
         "${_bioseq_e2e_vulkan_release_loader}"
      OR NOT "${_bioseq_e2e_vulkan_imported_loader}" STREQUAL
             "${_bioseq_e2e_vulkan_release_loader}"
      OR NOT _bioseq_e2e_vulkan_selected_debug_offset EQUAL -1
      OR NOT _bioseq_e2e_vulkan_imported_debug_offset EQUAL -1)
    message(FATAL_ERROR
      "Optimized Apple E2E Vulkan_LIBRARY and Vulkan::Vulkan must select the "
      "same receipt-owned release loader")
  endif()
  unset(_bioseq_e2e_vulkan_imported_debug_offset)
  unset(_bioseq_e2e_vulkan_selected_debug_offset)
  unset(_bioseq_e2e_vulkan_imported_loader)
  unset(_bioseq_e2e_vulkan_selected_loader)
  unset(_bioseq_e2e_vulkan_debug_offset)
  unset(_bioseq_e2e_vulkan_release_is_receipt_owned)
  unset(_bioseq_e2e_vulkan_receipt_root)
  unset(_bioseq_e2e_vulkan_release_loader)
  unset(_bioseq_e2e_vulkan_release_candidate)
endif()

set(BIOSEQ_GLSLANG_STATIC_LIBS
  glslang::glslang
  glslang::SPIRV
  glslang::OSDependent
  glslang::MachineIndependent
  glslang::GenericCodeGen)

foreach(_bioseq_glslang_target IN LISTS BIOSEQ_GLSLANG_STATIC_LIBS)
  if(NOT TARGET ${_bioseq_glslang_target})
    message(FATAL_ERROR
      "glslang did not expose required target ${_bioseq_glslang_target}; "
      "rerun vcpkg install glslang")
  endif()
endforeach()

# Benchmark target wiring is included near the end of this file;
# `bioseq_benchmark` consumes `bench_framework`.

# ============================================================================
# Cross-target BMI sharing uses an OBJECT library that owns the domain-neutral
# (NO_POSTGRES) module file set. Consumers link its PUBLIC CXX_MODULES file set
# and compile only their target-private modules.
#
# Catch sources stay target-private; this inventory contains production module
# units only, so non-test binaries never acquire Catch2 reporter headers.
# ============================================================================

# Domain-neutral membership is structural: the exact non-PostgreSQL backend
# subtrees plus instrumentation/**.
# PostgreSQL implementation units are reclassified below after the structural
# glob so named implementations remain ordinary private sources.
file(GLOB_RECURSE BIOSEQ_NOPG_MODULES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/storage/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/vector_compare/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  "${_bioseq_build_config_authored_input}")
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  ${BIOSEQ_GPU_ARCHIVE_MODULE_UNITS}
  ${BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS})
# Fail closed on authored additions until their target and generator ownership
# has been reviewed and the exact inventory count is advanced deliberately.
# DOC0433 adds pure minimizer layout geometry and the shared native heap
# allocation helper. The heap module belongs to the small runtime-core closure.
# The generic bounded ntHash seed policy is one shared, target-neutral module.
list(LENGTH BIOSEQ_NOPG_MODULES _bioseq_nopg_module_count)
if(NOT _bioseq_nopg_module_count EQUAL 349)
  message(FATAL_ERROR
        "Domain-neutral module inventory must contain exactly 349 authored units; "
    "got ${_bioseq_nopg_module_count}")
endif()
unset(_bioseq_nopg_module_count)

foreach(_bioseq_build_config_broad_inventory IN ITEMS
    SRC_MOD_FILES BIOSEQ_NOPG_MODULES)
  list(FIND ${_bioseq_build_config_broad_inventory}
    "${_bioseq_build_config_authored_input}"
    _bioseq_build_config_broad_owner_index)
  if(NOT _bioseq_build_config_broad_owner_index EQUAL -1)
    message(FATAL_ERROR
      "Authored build-config generator input escaped into broad module "
      "ownership: ${_bioseq_build_config_broad_inventory}")
  endif()
endforeach()
unset(_bioseq_build_config_broad_owner_index)
unset(_bioseq_build_config_broad_inventory)

# Highway generator inputs are authored module interfaces whose namespace and
# callable structure are validated by Clang. They are never final providers:
# bioseq_codegen emits one uniquely named production module graph per enabled
# Highway target. Keep this explicit ownership list out of ordinary module globs.
set(BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_utils.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_vector_stream_operations.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_vector_stream.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/highway_sequence_ops.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_base_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_lexic_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_minimap_int_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimap2_reference_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/state.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/resident_stream.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/exact_vectors.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/exact_decode.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/exact_k15_w10.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/exact_chronological.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/continuation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash/drivers.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/utils_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_primitives.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_dp.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_traceback.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_certificates.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/prefilter/sneaky_snake/kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_candidate_scoring.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_forward.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_backtracking.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/highway_indexing_kernels.cppm")

# Template-flatten inputs have the same exact-set ownership contract as
# Highway target templates. A marked source is scan-only generator input, never
# an ordinary module provider, and every registered input must carry the marker.
set(BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/typed_template.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_owners_template.cppm")

# The annotation and generator registry are one exact ownership boundary. Scan
# only authored source-tree module units (never current or stale build outputs),
# normalize both sets to deterministic real paths, and fail before either an
# annotated source becomes an ordinary production provider or an unannotated
# file is silently treated as a generator input.
file(GLOB_RECURSE _bioseq_highway_authored_cppm_candidates
  LIST_DIRECTORIES FALSE CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/*.cppm")
set(_bioseq_highway_marked_modules)
foreach(_bioseq_highway_candidate IN LISTS
    _bioseq_highway_authored_cppm_candidates)
  get_filename_component(_bioseq_highway_candidate
    "${_bioseq_highway_candidate}" REALPATH)
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_bioseq_highway_candidate}"
    NORMALIZE _bioseq_highway_in_root_binary_dir)
  cmake_path(IS_PREFIX CMAKE_CURRENT_BINARY_DIR "${_bioseq_highway_candidate}"
    NORMALIZE _bioseq_highway_in_local_binary_dir)
  file(RELATIVE_PATH _bioseq_highway_relative_candidate
    "${CMAKE_CURRENT_LIST_DIR}" "${_bioseq_highway_candidate}")
  if(_bioseq_highway_in_root_binary_dir
      OR _bioseq_highway_in_local_binary_dir
      OR _bioseq_highway_relative_candidate MATCHES
        "(^|/)(build[^/]*|cmake-build[^/]*|generated|CMakeFiles)(/|$)"
      OR _bioseq_highway_relative_candidate MATCHES
        "(^|/)(test|tests|integration_tests|bench|benches|benchmarks)(/|$)"
      OR _bioseq_highway_relative_candidate MATCHES
        "(^|/)[^/]*_(test|tests)\\.cppm$")
    continue()
  endif()

  file(READ "${_bioseq_highway_candidate}"
    _bioseq_highway_candidate_contents)
  string(REGEX MATCH
    "(^|[\r\n])[ \t]*#[ \t]*include[ \t]*[<\"][^>\"\r\n]+\\.cppm[>\"]"
    _bioseq_highway_cppm_include
    "${_bioseq_highway_candidate_contents}")
  if(_bioseq_highway_cppm_include)
    message(FATAL_ERROR
      "Production module units may not textually include another "
      ".cppm: ${_bioseq_highway_candidate}")
  endif()
  string(REGEX REPLACE "[ \t\r\n]+" ""
    _bioseq_highway_compact_contents
    "${_bioseq_highway_candidate_contents}")
  string(FIND "${_bioseq_highway_compact_contents}"
    "namespace[[bioseq::simd]]target"
    _bioseq_highway_marker_offset)
  if(NOT _bioseq_highway_marker_offset EQUAL -1)
    list(APPEND _bioseq_highway_marked_modules
      "${_bioseq_highway_candidate}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _bioseq_highway_marked_modules)
list(SORT _bioseq_highway_marked_modules)

set(_bioseq_highway_registered_modules)
set(_bioseq_highway_unmarked_registered_modules)
foreach(_bioseq_highway_registered IN LISTS
    BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES)
  if(NOT EXISTS "${_bioseq_highway_registered}")
    message(FATAL_ERROR
      "Highway generator input does not exist: "
      "${_bioseq_highway_registered}")
  endif()
  get_filename_component(_bioseq_highway_registered
    "${_bioseq_highway_registered}" REALPATH)
  cmake_path(IS_PREFIX CMAKE_CURRENT_LIST_DIR "${_bioseq_highway_registered}"
    NORMALIZE _bioseq_highway_registered_in_source)
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_bioseq_highway_registered}"
    NORMALIZE _bioseq_highway_registered_in_binary)
  if(NOT _bioseq_highway_registered_in_source
      OR _bioseq_highway_registered_in_binary)
    message(FATAL_ERROR
      "Highway generator inputs must be authored beneath "
      "${CMAKE_CURRENT_LIST_DIR}, not generated/build-tree files: "
      "${_bioseq_highway_registered}")
  endif()
  list(APPEND _bioseq_highway_registered_modules
    "${_bioseq_highway_registered}")
  list(FIND _bioseq_highway_marked_modules
    "${_bioseq_highway_registered}" _bioseq_highway_marked_index)
  if(_bioseq_highway_marked_index EQUAL -1)
    list(APPEND _bioseq_highway_unmarked_registered_modules
      "${_bioseq_highway_registered}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _bioseq_highway_registered_modules)
list(SORT _bioseq_highway_registered_modules)

set(_bioseq_highway_unregistered_marked_modules
  ${_bioseq_highway_marked_modules})
list(REMOVE_ITEM _bioseq_highway_unregistered_marked_modules
  ${_bioseq_highway_registered_modules})
if(_bioseq_highway_unregistered_marked_modules
    OR _bioseq_highway_unmarked_registered_modules
    OR NOT "${_bioseq_highway_marked_modules}" STREQUAL
      "${_bioseq_highway_registered_modules}")
  string(JOIN "\n    " _bioseq_highway_missing_text
    ${_bioseq_highway_unregistered_marked_modules})
  string(JOIN "\n    " _bioseq_highway_extra_text
    ${_bioseq_highway_unmarked_registered_modules})
  message(FATAL_ERROR
    "[[bioseq::simd]] template ownership registry is not an exact set.\n"
    "  Marked but unregistered:\n    ${_bioseq_highway_missing_text}\n"
    "  Registered but unmarked:\n    ${_bioseq_highway_extra_text}")
endif()

unset(_bioseq_highway_extra_text)
unset(_bioseq_highway_missing_text)
unset(_bioseq_highway_unregistered_marked_modules)
unset(_bioseq_highway_unmarked_registered_modules)
unset(_bioseq_highway_marked_modules)
unset(_bioseq_highway_marker_offset)
unset(_bioseq_highway_cppm_include)
unset(_bioseq_highway_compact_contents)
unset(_bioseq_highway_candidate_contents)
unset(_bioseq_highway_relative_candidate)
unset(_bioseq_highway_in_local_binary_dir)
unset(_bioseq_highway_in_root_binary_dir)
unset(_bioseq_highway_registered_in_binary)
unset(_bioseq_highway_registered_in_source)
unset(_bioseq_highway_registered)
unset(_bioseq_highway_candidate)
unset(_bioseq_highway_marked_index)
unset(_bioseq_highway_authored_cppm_candidates)

file(GLOB_RECURSE _bioseq_template_flatten_authored_cppm_candidates
  LIST_DIRECTORIES FALSE CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/*.cppm")
set(_bioseq_template_flatten_marked_modules)
foreach(_bioseq_template_flatten_candidate IN LISTS
    _bioseq_template_flatten_authored_cppm_candidates)
  get_filename_component(_bioseq_template_flatten_candidate
    "${_bioseq_template_flatten_candidate}" REALPATH)
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR
    "${_bioseq_template_flatten_candidate}"
    NORMALIZE _bioseq_template_flatten_in_root_binary_dir)
  cmake_path(IS_PREFIX CMAKE_CURRENT_BINARY_DIR
    "${_bioseq_template_flatten_candidate}"
    NORMALIZE _bioseq_template_flatten_in_local_binary_dir)
  file(RELATIVE_PATH _bioseq_template_flatten_relative_candidate
    "${CMAKE_CURRENT_LIST_DIR}" "${_bioseq_template_flatten_candidate}")
  if(_bioseq_template_flatten_in_root_binary_dir
      OR _bioseq_template_flatten_in_local_binary_dir
      OR _bioseq_template_flatten_relative_candidate MATCHES
        "(^|/)(build[^/]*|cmake-build[^/]*|generated|CMakeFiles)(/|$)"
      OR _bioseq_template_flatten_relative_candidate MATCHES
        "(^|/)(test|tests|integration_tests|bench|benches|benchmarks)(/|$)"
      OR _bioseq_template_flatten_relative_candidate MATCHES
        "(^|/)[^/]*_(test|tests)\\.cppm$")
    continue()
  endif()

  file(READ "${_bioseq_template_flatten_candidate}"
    _bioseq_template_flatten_candidate_contents)
  string(REGEX REPLACE "[ \t\r\n]+" ""
    _bioseq_template_flatten_compact_contents
    "${_bioseq_template_flatten_candidate_contents}")
  string(FIND "${_bioseq_template_flatten_compact_contents}"
    "[[bioseq::template_flatten("
    _bioseq_template_flatten_marker_offset)
  if(NOT _bioseq_template_flatten_marker_offset EQUAL -1)
    list(APPEND _bioseq_template_flatten_marked_modules
      "${_bioseq_template_flatten_candidate}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _bioseq_template_flatten_marked_modules)
list(SORT _bioseq_template_flatten_marked_modules)

set(_bioseq_template_flatten_registered_modules)
set(_bioseq_template_flatten_unmarked_registered_modules)
foreach(_bioseq_template_flatten_registered IN LISTS
    BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES)
  if(NOT EXISTS "${_bioseq_template_flatten_registered}")
    message(FATAL_ERROR
      "Template-flatten generator input does not exist: "
      "${_bioseq_template_flatten_registered}")
  endif()
  get_filename_component(_bioseq_template_flatten_registered
    "${_bioseq_template_flatten_registered}" REALPATH)
  cmake_path(IS_PREFIX CMAKE_CURRENT_LIST_DIR
    "${_bioseq_template_flatten_registered}"
    NORMALIZE _bioseq_template_flatten_registered_in_source)
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR
    "${_bioseq_template_flatten_registered}"
    NORMALIZE _bioseq_template_flatten_registered_in_binary)
  if(NOT _bioseq_template_flatten_registered_in_source
      OR _bioseq_template_flatten_registered_in_binary)
    message(FATAL_ERROR
      "Template-flatten inputs must be authored beneath "
      "${CMAKE_CURRENT_LIST_DIR}, not generated/build-tree files: "
      "${_bioseq_template_flatten_registered}")
  endif()
  list(APPEND _bioseq_template_flatten_registered_modules
    "${_bioseq_template_flatten_registered}")
  list(FIND _bioseq_template_flatten_marked_modules
    "${_bioseq_template_flatten_registered}"
    _bioseq_template_flatten_marked_index)
  if(_bioseq_template_flatten_marked_index EQUAL -1)
    list(APPEND _bioseq_template_flatten_unmarked_registered_modules
      "${_bioseq_template_flatten_registered}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _bioseq_template_flatten_registered_modules)
list(SORT _bioseq_template_flatten_registered_modules)

foreach(_bioseq_template_flatten_registered IN LISTS
    _bioseq_template_flatten_registered_modules)
  list(FIND _bioseq_highway_registered_modules
    "${_bioseq_template_flatten_registered}"
    _bioseq_cross_codegen_source_index)
  if(NOT _bioseq_cross_codegen_source_index EQUAL -1)
    message(FATAL_ERROR
      "Highway and template-flatten generator source sets must be "
      "disjoint: ${_bioseq_template_flatten_registered}")
  endif()
endforeach()

set(_bioseq_template_flatten_unregistered_marked_modules
  ${_bioseq_template_flatten_marked_modules})
list(REMOVE_ITEM _bioseq_template_flatten_unregistered_marked_modules
  ${_bioseq_template_flatten_registered_modules})
if(_bioseq_template_flatten_unregistered_marked_modules
    OR _bioseq_template_flatten_unmarked_registered_modules
    OR NOT "${_bioseq_template_flatten_marked_modules}" STREQUAL
      "${_bioseq_template_flatten_registered_modules}")
  string(JOIN "\n    " _bioseq_template_flatten_missing_text
    ${_bioseq_template_flatten_unregistered_marked_modules})
  string(JOIN "\n    " _bioseq_template_flatten_extra_text
    ${_bioseq_template_flatten_unmarked_registered_modules})
  message(FATAL_ERROR
    "[[bioseq::template_flatten]] ownership registry is not an exact set.\n"
    "  Marked but unregistered:\n    ${_bioseq_template_flatten_missing_text}\n"
    "  Registered but unmarked:\n    ${_bioseq_template_flatten_extra_text}")
endif()

unset(_bioseq_template_flatten_extra_text)
unset(_bioseq_template_flatten_missing_text)
unset(_bioseq_template_flatten_unregistered_marked_modules)
unset(_bioseq_template_flatten_unmarked_registered_modules)
unset(_bioseq_template_flatten_registered_modules)
unset(_bioseq_highway_registered_modules)
unset(_bioseq_cross_codegen_source_index)
unset(_bioseq_template_flatten_marked_modules)
unset(_bioseq_template_flatten_marker_offset)
unset(_bioseq_template_flatten_compact_contents)
unset(_bioseq_template_flatten_candidate_contents)
unset(_bioseq_template_flatten_relative_candidate)
unset(_bioseq_template_flatten_in_local_binary_dir)
unset(_bioseq_template_flatten_in_root_binary_dir)
unset(_bioseq_template_flatten_registered_in_binary)
unset(_bioseq_template_flatten_registered_in_source)
unset(_bioseq_template_flatten_registered)
unset(_bioseq_template_flatten_candidate)
unset(_bioseq_template_flatten_marked_index)
unset(_bioseq_template_flatten_authored_cppm_candidates)

# The generated scheduler first erases physical storage width to
# lazy_sequence_view<alphabet>. Own every heavy pipeline closure after that
# boundary exactly once in PostgreSQL-DSO-private ordinary implementation units
# of bioseq.pipeline.pipe. The canonical scan-only source describes alphabet x
# route; only the profile-reachable generated subset is built, and no Domain-N
# executable links these scheduler-only definitions.
set(BIOSEQ_PIPELINE_DISPATCH_OWNER_TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_owners_template.cppm")
set(BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/template_flatten/pipe_owners")

# The former directory contained one manually authored owner per coordinate.
# It is retired: adding a source there would silently create a second owner.
file(GLOB _bioseq_pipeline_dispatch_owner_authored CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe/*.cppm")
if(_bioseq_pipeline_dispatch_owner_authored)
  message(FATAL_ERROR
    "Retired authored pipeline-owner directory must remain empty: "
    "${_bioseq_pipeline_dispatch_owner_authored}")
endif()
unset(_bioseq_pipeline_dispatch_owner_authored)

bioseq_template_flatten_probe(
  NAME pipeline_pipe_owners
  SOURCE "${BIOSEQ_PIPELINE_DISPATCH_OWNER_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.pipeline.pipe
  PROFILE "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}"
  EMISSION_POLICY implementation_owners
  OUT_CONFIGURE_TOPOLOGY_MANIFEST
    BIOSEQ_PIPELINE_DISPATCH_OWNER_CONFIGURE_TOPOLOGY
  OUT_TOPOLOGY_DIGEST BIOSEQ_PIPELINE_DISPATCH_OWNER_TOPOLOGY_DIGEST
  OUT_ENABLED_ORDINALS BIOSEQ_PIPELINE_DISPATCH_OWNER_ENABLED_ORDINALS
  OUT_ENABLED_SOURCES
    BIOSEQ_PIPELINE_DISPATCH_OWNER_PROBED_IMPLEMENTATION_UNITS)

set(BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA4_IMPLEMENTATION_UNITS)
foreach(_bioseq_pipeline_dispatch_owner_ordinal RANGE 0 5)
  list(APPEND BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA4_IMPLEMENTATION_UNITS
    "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}/instance_${_bioseq_pipeline_dispatch_owner_ordinal}.cppm")
endforeach()
set(BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA5_IMPLEMENTATION_UNITS)
foreach(_bioseq_pipeline_dispatch_owner_ordinal RANGE 6 11)
  list(APPEND BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA5_IMPLEMENTATION_UNITS
    "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}/instance_${_bioseq_pipeline_dispatch_owner_ordinal}.cppm")
endforeach()
unset(_bioseq_pipeline_dispatch_owner_ordinal)
set(BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA4_IMPLEMENTATION_UNITS}
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA5_IMPLEMENTATION_UNITS})

list(LENGTH BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA4_IMPLEMENTATION_UNITS
  _bioseq_pipeline_dispatch_owner_dna4_count)
list(LENGTH BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA5_IMPLEMENTATION_UNITS
  _bioseq_pipeline_dispatch_owner_dna5_count)
list(LENGTH BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS
  _bioseq_pipeline_dispatch_owner_all_count)
if(NOT _bioseq_pipeline_dispatch_owner_dna4_count EQUAL 6
    OR NOT _bioseq_pipeline_dispatch_owner_dna5_count EQUAL 6
    OR NOT _bioseq_pipeline_dispatch_owner_all_count EQUAL 12)
  message(FATAL_ERROR
    "Post-erasure pipeline owner registry must contain exactly six dna4 and "
    "six dna5 units (12 total)")
endif()
set(_bioseq_pipeline_dispatch_owner_unique
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS})
list(REMOVE_DUPLICATES _bioseq_pipeline_dispatch_owner_unique)
list(LENGTH _bioseq_pipeline_dispatch_owner_unique
  _bioseq_pipeline_dispatch_owner_unique_count)
if(NOT _bioseq_pipeline_dispatch_owner_unique_count EQUAL 12)
  message(FATAL_ERROR
    "Post-erasure pipeline owner registry contains duplicate paths")
endif()
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  cmake_path(IS_PREFIX BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT
    "${_bioseq_pipeline_dispatch_owner}" NORMALIZE
    _bioseq_pipeline_dispatch_owner_in_generated_root)
  if(NOT _bioseq_pipeline_dispatch_owner_in_generated_root)
    message(FATAL_ERROR
      "Pipeline owner escaped its generated root: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner_in_generated_root)
unset(_bioseq_pipeline_dispatch_owner_unique_count)
unset(_bioseq_pipeline_dispatch_owner_unique)
unset(_bioseq_pipeline_dispatch_owner_all_count)
unset(_bioseq_pipeline_dispatch_owner_dna5_count)
unset(_bioseq_pipeline_dispatch_owner_dna4_count)

# Generated scheduler sources and their imported typed-support closure must
# reach the heavy pipeline only through the distinct scheduler-owned entries.
# A compatibility-template call or explicit template-id would silently restore
# one heavy instantiation per leaf.
set(_bioseq_pipeline_dispatch_owner_boundary_root
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor")
set(_bioseq_pipeline_dispatch_owner_boundary_support_root
  "${_bioseq_pipeline_dispatch_owner_boundary_root}/typed_support_parts")
set(_bioseq_pipeline_dispatch_owner_boundary_sources
  "${_bioseq_pipeline_dispatch_owner_boundary_root}/typed_template.cppm"
  "${_bioseq_pipeline_dispatch_owner_boundary_root}/typed_support.cppm")
file(GLOB _bioseq_pipeline_dispatch_owner_boundary_support_sources
  CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
  "${_bioseq_pipeline_dispatch_owner_boundary_support_root}/*.cppm")
list(APPEND _bioseq_pipeline_dispatch_owner_boundary_sources
  ${_bioseq_pipeline_dispatch_owner_boundary_support_sources})
set(_bioseq_pipeline_dispatch_owner_boundary_unique_sources
  ${_bioseq_pipeline_dispatch_owner_boundary_sources})
list(REMOVE_DUPLICATES
  _bioseq_pipeline_dispatch_owner_boundary_unique_sources)
list(LENGTH _bioseq_pipeline_dispatch_owner_boundary_sources
  _bioseq_pipeline_dispatch_owner_boundary_source_count)
list(LENGTH _bioseq_pipeline_dispatch_owner_boundary_unique_sources
  _bioseq_pipeline_dispatch_owner_boundary_unique_source_count)
if(NOT _bioseq_pipeline_dispatch_owner_boundary_source_count EQUAL
    _bioseq_pipeline_dispatch_owner_boundary_unique_source_count)
  message(FATAL_ERROR
    "Post-erasure scheduler boundary contains duplicate sources")
endif()
if(NOT _bioseq_pipeline_dispatch_owner_boundary_source_count EQUAL 17)
  message(FATAL_ERROR
    "Post-erasure scheduler boundary must contain exactly 17 sources; got "
    "${_bioseq_pipeline_dispatch_owner_boundary_source_count}")
endif()
set(_bioseq_pipeline_dispatch_owner_boundary_text)
foreach(_bioseq_pipeline_dispatch_owner_boundary_source IN LISTS
    _bioseq_pipeline_dispatch_owner_boundary_sources)
  if(NOT EXISTS "${_bioseq_pipeline_dispatch_owner_boundary_source}")
    message(FATAL_ERROR
      "Post-erasure scheduler boundary source does not exist: "
      "${_bioseq_pipeline_dispatch_owner_boundary_source}")
  endif()
  if(NOT _bioseq_pipeline_dispatch_owner_boundary_source MATCHES
      "\\.cppm$")
    message(FATAL_ERROR
      "Post-erasure scheduler boundary source is not a module unit: "
      "${_bioseq_pipeline_dispatch_owner_boundary_source}")
  endif()
  if(NOT _bioseq_pipeline_dispatch_owner_boundary_source STREQUAL
        "${_bioseq_pipeline_dispatch_owner_boundary_root}/typed_template.cppm"
      AND NOT _bioseq_pipeline_dispatch_owner_boundary_source STREQUAL
        "${_bioseq_pipeline_dispatch_owner_boundary_root}/typed_support.cppm")
    cmake_path(IS_PREFIX
      _bioseq_pipeline_dispatch_owner_boundary_support_root
      "${_bioseq_pipeline_dispatch_owner_boundary_source}" NORMALIZE
      _bioseq_pipeline_dispatch_owner_boundary_source_in_support_root)
    file(RELATIVE_PATH
      _bioseq_pipeline_dispatch_owner_boundary_relative_source
      "${_bioseq_pipeline_dispatch_owner_boundary_support_root}"
      "${_bioseq_pipeline_dispatch_owner_boundary_source}")
    if(NOT _bioseq_pipeline_dispatch_owner_boundary_source_in_support_root
        OR _bioseq_pipeline_dispatch_owner_boundary_relative_source MATCHES
          "/")
      message(FATAL_ERROR
        "Post-erasure scheduler boundary source escaped its exact category: "
        "${_bioseq_pipeline_dispatch_owner_boundary_source}")
    endif()
  endif()
  file(READ "${_bioseq_pipeline_dispatch_owner_boundary_source}"
    _bioseq_pipeline_dispatch_owner_boundary_source_text)
  string(APPEND _bioseq_pipeline_dispatch_owner_boundary_text
    "\n${_bioseq_pipeline_dispatch_owner_boundary_source_text}")
endforeach()
foreach(_bioseq_pipeline_dispatch_owner_bypass_pattern IN ITEMS
    "minimap_port[ \t\r\n]*\\("
    "minimap_port_segmented[ \t\r\n]*\\("
    "minimap2_port_with_index[ \t\r\n]*\\("
    "minimap_port[ \t\r\n]*<"
    "minimap_port_segmented[ \t\r\n]*<"
    "minimap2_port_with_index[ \t\r\n]*<"
    "minimap_port_owned[ \t\r\n]*\\("
    "minimap2_port_with_index_owned[ \t\r\n]*\\("
    "minimap2_port_with_index_owned[ \t\r\n]*<"
    "dispatch_pipeline[ \t\r\n]*<"
    "minimap_port_with_repr[ \t\r\n]*<")
  if(_bioseq_pipeline_dispatch_owner_boundary_text MATCHES
      "${_bioseq_pipeline_dispatch_owner_bypass_pattern}")
    message(FATAL_ERROR
      "Generated scheduler input bypasses the post-erasure pipeline owner "
      "boundary with pattern '${_bioseq_pipeline_dispatch_owner_bypass_pattern}'")
  endif()
endforeach()
foreach(_bioseq_pipeline_dispatch_owned_entry IN ITEMS
    minimap_port_raw_owned
    minimap_port_segmented_owned
    minimap_port_fragment_index_vector_owned
    minimap_port_fragment_index_physical_owned
    minimap_port_hash_major_owned
    minimap_port_grouped_soa_owned
    minimap_port_fragment_query_owned)
  if(NOT _bioseq_pipeline_dispatch_owner_boundary_text MATCHES
      "${_bioseq_pipeline_dispatch_owned_entry}[ \t\r\n]*\\(")
    message(FATAL_ERROR
      "Generated scheduler input does not call required post-erasure entry "
      "'${_bioseq_pipeline_dispatch_owned_entry}'")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owned_entry)
unset(_bioseq_pipeline_dispatch_owner_bypass_pattern)
unset(_bioseq_pipeline_dispatch_owner_boundary_source_text)
unset(_bioseq_pipeline_dispatch_owner_boundary_relative_source)
unset(_bioseq_pipeline_dispatch_owner_boundary_source_in_support_root)
unset(_bioseq_pipeline_dispatch_owner_boundary_unique_source_count)
unset(_bioseq_pipeline_dispatch_owner_boundary_source_count)
unset(_bioseq_pipeline_dispatch_owner_boundary_unique_sources)
unset(_bioseq_pipeline_dispatch_owner_boundary_support_sources)
unset(_bioseq_pipeline_dispatch_owner_boundary_sources)
unset(_bioseq_pipeline_dispatch_owner_boundary_text)
unset(_bioseq_pipeline_dispatch_owner_boundary_support_root)
unset(_bioseq_pipeline_dispatch_owner_boundary_root)

set(_bioseq_pipeline_dispatch_owner_expected_implementation_units
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA4_IMPLEMENTATION_UNITS})
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
  list(APPEND _bioseq_pipeline_dispatch_owner_expected_implementation_units
    ${BIOSEQ_PIPELINE_DISPATCH_OWNER_DNA5_IMPLEMENTATION_UNITS})
endif()
if(NOT "${BIOSEQ_PIPELINE_DISPATCH_OWNER_PROBED_IMPLEMENTATION_UNITS}"
    STREQUAL
    "${_bioseq_pipeline_dispatch_owner_expected_implementation_units}")
  message(FATAL_ERROR
    "Pipeline-owner profile projection diverges from the canonical "
    "alphabet x route topology")
endif()

# Define the target-neutral registry before passing its stamp to PREPARE.
# Defining it later in the Highway target graph made this dependency expand
# to an empty string on cold builds, so the generated provider was missing
# when the pipeline-owner import closure was inspected.
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
  set(BIOSEQ_MINIMIZER_REGISTRY_PROFILE extended)
else()
  set(BIOSEQ_MINIMIZER_REGISTRY_PROFILE baseline)
endif()
set(BIOSEQ_MINIMIZER_REGISTRY_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/minimizer_registry")
bioseq_add_minimizer_registry_codegen(
  TARGET bioseq_minimizer_registry_modules
  OUTPUT_ROOT "${BIOSEQ_MINIMIZER_REGISTRY_GENERATED_ROOT}"
  PROFILE "${BIOSEQ_MINIMIZER_REGISTRY_PROFILE}"
  OUTPUT_RECORDS_MODULE BIOSEQ_MINIMIZER_PHYSICAL_RECORDS_MODULE
  OUTPUT_VISITOR_MODULE BIOSEQ_MINIMIZER_PHYSICAL_VISITOR_MODULE
  OUTPUT_MANIFEST BIOSEQ_MINIMIZER_REGISTRY_MANIFEST
  OUTPUT_STAMP BIOSEQ_MINIMIZER_REGISTRY_STAMP)

bioseq_add_template_flatten_codegen(
  NAME pipeline_pipe_owners
  SOURCE "${BIOSEQ_PIPELINE_DISPATCH_OWNER_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.pipeline.pipe
  EMISSION_POLICY implementation_owners
  CONFIGURE_TOPOLOGY_MANIFEST
    "${BIOSEQ_PIPELINE_DISPATCH_OWNER_CONFIGURE_TOPOLOGY}"
  EXPECTED_TOPOLOGY_DIGEST
    "${BIOSEQ_PIPELINE_DISPATCH_OWNER_TOPOLOGY_DIGEST}"
  ENABLED_ORDINALS ${BIOSEQ_PIPELINE_DISPATCH_OWNER_ENABLED_ORDINALS}
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe.cppm"
  PCM_ROOT "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}/pcm"
  PREPARE_DEPENDS "${BIOSEQ_MINIMIZER_REGISTRY_STAMP}"
  OUT_IMPLEMENTATION_UNITS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS
  OUT_GENERATION_TARGET BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATION_TARGET
  OUT_ARTIFACT_MANIFEST BIOSEQ_PIPELINE_DISPATCH_OWNER_ARTIFACT_MANIFEST)

if(NOT "${BIOSEQ_PIPELINE_DISPATCH_OWNER_PROBED_IMPLEMENTATION_UNITS}"
    STREQUAL "${BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS}")
  message(FATAL_ERROR
    "Pipeline-owner probe and generator disagree on enabled units")
endif()

list(LENGTH BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS
  _bioseq_pipeline_dispatch_owner_count)
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
  set(_bioseq_expected_pipeline_dispatch_owner_count 12)
else()
  set(_bioseq_expected_pipeline_dispatch_owner_count 6)
endif()
if(NOT _bioseq_pipeline_dispatch_owner_count EQUAL
    _bioseq_expected_pipeline_dispatch_owner_count)
  message(FATAL_ERROR
    "Post-erasure pipeline owner cardinality mismatch: expected "
    "${_bioseq_expected_pipeline_dispatch_owner_count}, got "
    "${_bioseq_pipeline_dispatch_owner_count}")
endif()
unset(_bioseq_expected_pipeline_dispatch_owner_count)
unset(_bioseq_pipeline_dispatch_owner_count)
unset(_bioseq_pipeline_dispatch_owner_expected_implementation_units)

set(BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_dispatch_contract_impl.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/sorting/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/simd_implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/vector_compare/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/instrumentation/summary_writer/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_hashmap/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_sequence/pair_sort.cppm")
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  if(_bioseq_pipeline_dispatch_owner IN_LIST
      BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS)
    message(FATAL_ERROR
      "Scheduler-only pipeline owner leaked into bioseq_modules_nopg: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
# The public PMU facade defines inline dispatch through each platform backend,
# so importers require these BMIs as part of the public module closure. Their
# backend namespaces remain implementation detail; CMake visibility here
# describes BMI reachability rather than API stability.
set(BIOSEQ_INSTRUMENTATION_BACKEND_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/linux.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/macos.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/instrumentation/pmu/unsupported.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  ${BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES}
  ${BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES}
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS}
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS}
  ${BIOSEQ_INSTRUMENTATION_BACKEND_MODULES})
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES}
  ${BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES}
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS}
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS}
  ${BIOSEQ_INSTRUMENTATION_BACKEND_MODULES})
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  list(FIND BIOSEQ_NOPG_MODULES "${_bioseq_pipeline_dispatch_owner}"
    _bioseq_pipeline_dispatch_owner_nopg_module_index)
  list(FIND SRC_MOD_FILES "${_bioseq_pipeline_dispatch_owner}"
    _bioseq_pipeline_dispatch_owner_pg_module_index)
  if(NOT _bioseq_pipeline_dispatch_owner_nopg_module_index EQUAL -1
      OR NOT _bioseq_pipeline_dispatch_owner_pg_module_index EQUAL -1)
    message(FATAL_ERROR
      "Ordinary pipeline owner leaked into a CXX_MODULES interface set: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner_pg_module_index)
unset(_bioseq_pipeline_dispatch_owner_nopg_module_index)
unset(_bioseq_pipeline_dispatch_owner)

# The IO reader closure is PostgreSQL-independent and is shared by the
# extension, external providers, and the IO benchmark. The benchmark launcher
# and logic module are target-private and removed from this broad owner below.
bioseq_collect_authored_sources(
  BIOSEQ_IO_MODULES
  BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/io"
  RECURSE
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS
    "benchmark/common.cppm"
    "benchmark/index.cppm"
    "benchmark/lazy_expression.cppm"
    "benchmark/main.cppm"
    "benchmark/readers.cppm"
    "benchmark/writers.cppm"
  EXPECTED_COUNT 34)
list(APPEND BIOSEQ_NOPG_MODULES ${BIOSEQ_IO_MODULES})
list(REMOVE_DUPLICATES BIOSEQ_NOPG_MODULES)

set(BIOSEQ_TBB_RUNTIME_SYMBOLS_IMPLEMENTATION
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/pool/tbb_runtime_symbols.cppm)
set_source_files_properties(
  ${BIOSEQ_TBB_RUNTIME_SYMBOLS_IMPLEMENTATION}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
set(BIOSEQ_RUNTIME_CORE_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/hashing/index.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/index.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/fmtlog_backend.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/fmtlog_backend/core.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/fmtlog_backend/runtime.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/formatters.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/context.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/heap_allocation.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/clock.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/concurrency_mode.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/concepts.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/pool.cppm)

# The external API client needs Bioseq allocation and worker-pool contracts,
# but not sequence/index/dispatch modules. Keep one canonical provider for this
# small runtime closure and share it with the full Domain-N library.
set(BIOSEQ_NOPG_OWNED_MODULES ${BIOSEQ_NOPG_MODULES})
set(BIOSEQ_VULKAN_RUNTIME_PARTITION_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/vulkan/runtime/coordinate_pipeline/batch_preparation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/vulkan/runtime/metal_archive/archive_storage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/vulkan/runtime/metal_archive/command_interpose.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/vulkan/runtime/metal_archive/pipeline_interpose.cppm")
foreach(_bioseq_vulkan_runtime_partition IN LISTS
    BIOSEQ_VULKAN_RUNTIME_PARTITION_MODULES)
  if(NOT _bioseq_vulkan_runtime_partition IN_LIST BIOSEQ_NOPG_OWNED_MODULES)
    message(FATAL_ERROR
      "Vulkan runtime partition escaped domain-neutral ownership: "
      "${_bioseq_vulkan_runtime_partition}")
  endif()
endforeach()
unset(_bioseq_vulkan_runtime_partition)
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  ${BIOSEQ_VULKAN_RUNTIME_PARTITION_MODULES})
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  list(FIND BIOSEQ_NOPG_OWNED_MODULES
    "${_bioseq_pipeline_dispatch_owner}"
    _bioseq_pipeline_dispatch_owner_public_index)
  if(NOT _bioseq_pipeline_dispatch_owner_public_index EQUAL -1)
    message(FATAL_ERROR
      "Ordinary pipeline owner leaked into the public CXX_MODULES file set: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner_public_index)
unset(_bioseq_pipeline_dispatch_owner)
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES ${BIOSEQ_RUNTIME_CORE_MODULES})

# Single-owner leaf targets stay in BIOSEQ_NOPG_MODULES so the PostgreSQL glob
# excludes them, but leave BIOSEQ_NOPG_OWNED_MODULES so they compile exactly
# once without runtime, TBB, logging, I/O, or PostgreSQL edges. Generated and
# provider-metadata targets link these leaves without inheriting the sequence
# graph. Checked arithmetic
# is owned tree-wide by common/checked_arithmetic and is therefore not part of
# the Bioseq source glob.
set(BIOSEQ_SINK_CONTROL_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/sink_control.cppm")
set(BIOSEQ_HTTP_VALIDATION_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/http/validation/index.cppm")
# The byte-source protocol remains a lightweight leaf so external transfer and
# generated clients do not inherit the sequence/alphabet graph.
set(BIOSEQ_IO_SOURCE_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/io/source.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  "${BIOSEQ_SINK_CONTROL_MODULE}"
  "${BIOSEQ_HTTP_VALIDATION_MODULE}"
  "${BIOSEQ_IO_SOURCE_MODULE}")

add_library(bioseq_sink_control_core STATIC)
set_target_properties(bioseq_sink_control_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_sink_control_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_sink_control_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_SINK_CONTROL_MODULE}")

add_library(bioseq_http_validation_core STATIC)
set_target_properties(bioseq_http_validation_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_http_validation_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_http_validation_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_HTTP_VALIDATION_MODULE}")

add_library(bioseq_io_source STATIC)
set_target_properties(bioseq_io_source PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_io_source PRIVATE NO_POSTGRES=1)
target_sources(bioseq_io_source
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_IO_SOURCE_MODULE}")

# The admission controller is a single-owner leaf, while OS probes and
# MemoryBudget share one implementation in bioseq_memory_budget. The sysinfo
# forwarding facade remains in the broad domain-neutral module set.
set(BIOSEQ_MEMORY_ADMISSION_CORE_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/memory_admission.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  ${BIOSEQ_MEMORY_ADMISSION_CORE_MODULES})

add_library(bioseq_memory_admission_core STATIC)
set_target_properties(bioseq_memory_admission_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_memory_admission_core PRIVATE NO_POSTGRES=1)
target_link_libraries(bioseq_memory_admission_core PUBLIC
  bioseq_checked_arithmetic_core
  bioseq_memory_budget)
target_sources(bioseq_memory_admission_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_MEMORY_ADMISSION_CORE_MODULES})

# Lightweight templating core: value sequence, compile-time string,
# pack iterate/search, and one-time runtime dispatch. Links Highway (required by
# pack_for_each) but has no sequence, alphabet, PG, or external edge. The
# Domain-N umbrella (bioseq.common.templating) and its remaining leaves keep
# their ownership and link this core for the moved leaves' BMIs.
set(BIOSEQ_COMMON_TEMPLATING_CORE_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/value_sequence.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/templating/compile_time_string.cppm"
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
  bioseq_checked_arithmetic_core
  ${CMAKE_DL_LIBS}
  fmt
  reflectcpp::reflectcpp
  foonathan_memory
  TBB::tbb)
target_sources(bioseq_runtime_core
  PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/specializations.cppm
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_RUNTIME_CORE_MODULES})
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/specializations.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/specializations.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/specializations.cppm")
set_source_files_properties(
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/specializations.cppm"
  PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    COMPILE_OPTIONS "-x;c++")

list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_TBB_RUNTIME_SYMBOLS_IMPLEMENTATION})
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  ${BIOSEQ_TBB_RUNTIME_SYMBOLS_IMPLEMENTATION})
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  ${BIOSEQ_TBB_RUNTIME_SYMBOLS_IMPLEMENTATION})

set(BIOSEQ_NOPG_SOURCES ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS})
set_source_files_properties(
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  list(APPEND BIOSEQ_NOPG_SOURCES ${BIOSEQ_TBB_RUNTIME_SYMBOLS_IMPLEMENTATION})
endif()
list(REMOVE_ITEM SRC_FILES ${BIOSEQ_NOPG_SOURCES})
list(REMOVE_ITEM SRC_FILES
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS})

add_library(bioseq_modules_nopg OBJECT)
set_target_properties(bioseq_modules_nopg PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  # IPO disabled here for the same reason as the test executables
  # variants (see add_postgresql_extension.cmake, comment on
  # `INTERPROCEDURAL_OPTIMIZATION FALSE` for `_tests_exec`): clang's
  # ThinLTO drops module-attached TLS init wrappers (the `_ZTHW…` symbols
  # for `mlogging::fmtlogDetailT<0>::sbc`) at link time. With the lib's
  # objects feeding into every Domain N consumer — including
  # `bioseq_unit_tests` which we know reproduces the bug — the
  # lib has to stay non-LTO so the test execs link cleanly. The lib's
  # codegen remains `-O3 -DNDEBUG` per bench-opt; only cross-TU inlining
  # between the library objects and consumer translation units is disabled.
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
# add_postgresql_extension.cmake's `target_compile_definitions(... PUBLIC -DNO_POSTGRES=1)`
# call for non-POSTGRES_EXTENSION targets, so the private scope here is sufficient
# for aligner and test consumers. postgresql_bioseq also consumes this library
# but MUST NOT receive NO_POSTGRES — its
# Domain P modules include <postgres.h> and rely on palloc, ereport, etc. The
# POSTGRESQL_VERSION_MAJOR define stays PUBLIC because both Domain N and
# Domain P consumers care about the major-version-conditional code paths.
target_compile_definitions(bioseq_modules_nopg PRIVATE NO_POSTGRES=1)
target_compile_definitions(bioseq_modules_nopg PUBLIC
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
if(APPLE)
  if(NOT DEFINED Vulkan_LIBRARY OR NOT IS_ABSOLUTE "${Vulkan_LIBRARY}"
      OR NOT DEFINED VCPKG_INSTALLED_DIR
      OR NOT IS_ABSOLUTE "${VCPKG_INSTALLED_DIR}"
      OR NOT DEFINED VCPKG_TARGET_TRIPLET
      OR "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    message(FATAL_ERROR
      "Apple Vulkan runtime requires one absolute CMake-selected vcpkg loader")
  endif()
  file(REAL_PATH "${Vulkan_LIBRARY}"
    _bioseq_receipt_owned_vulkan_loader)
  file(REAL_PATH
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
    _bioseq_receipt_owned_vulkan_root)
  cmake_path(IS_PREFIX _bioseq_receipt_owned_vulkan_root
    "${_bioseq_receipt_owned_vulkan_loader}" NORMALIZE
    _bioseq_vulkan_loader_is_receipt_owned)
  string(FIND "${_bioseq_receipt_owned_vulkan_loader}" "/debug/lib/"
    _bioseq_vulkan_loader_debug_offset)
  if(NOT _bioseq_vulkan_loader_is_receipt_owned
      OR NOT EXISTS "${_bioseq_receipt_owned_vulkan_loader}"
      OR (BIOSEQ_BENCHMARK_E2E_INTERVALS
          AND NOT _bioseq_vulkan_loader_debug_offset EQUAL -1))
    message(FATAL_ERROR
      "CMake-selected Vulkan loader is outside the permitted receipt-owned "
      "vcpkg tree or is a debug image in an optimized E2E receipt: "
      "${_bioseq_receipt_owned_vulkan_loader}")
  endif()
  target_compile_definitions(bioseq_modules_nopg PUBLIC
    "BIOSEQ_RECEIPT_OWNED_VULKAN_LOADER_PATH=\"${_bioseq_receipt_owned_vulkan_loader}\"")
  unset(_bioseq_vulkan_loader_is_receipt_owned)
  unset(_bioseq_vulkan_loader_debug_offset)
  unset(_bioseq_receipt_owned_vulkan_root)
  unset(_bioseq_receipt_owned_vulkan_loader)
endif()
target_compile_options(bioseq_modules_nopg PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")

target_include_directories(bioseq_modules_nopg PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include"
  "${CMAKE_CURRENT_LIST_DIR}/backend"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/sorting/hwy_contrib"
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
  bioseq_build_config
  bioseq_memory_budget
  bioseq_runtime_core
  bioseq_sink_control_core
  bioseq_http_validation_core
  bioseq_checked_arithmetic_core
  bioseq_io_source
  bioseq_memory_admission_core
  bioseq_common_templating_core
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
  PUBLIC FILE_SET bioseq_instrumentation_backend_modules
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_INSTRUMENTATION_BACKEND_MODULES}
  PUBLIC FILE_SET bioseq_vulkan_runtime_partition_modules
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_VULKAN_RUNTIME_PARTITION_MODULES}
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_NOPG_OWNED_MODULES})

# Scheduler dispatch owners are ordinary implementation units of the public
# bioseq.pipeline.pipe module. Keep their CXX dyndep small and DSO-private:
# this OBJECT target waits only for the Domain-N provider/BMI closure and is
# linked only by postgresql_bioseq below. In particular, it has no target-wide
# dependency on typed-dispatch generation, SQL validation, or Highway scans.
set_source_files_properties(
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS}
  PROPERTIES
    GENERATED TRUE
    CXX_SCAN_FOR_MODULES ON
    COMPILE_OPTIONS "-x;c++")
add_library(bioseq_pipeline_dispatch_owners OBJECT)
set_target_properties(bioseq_pipeline_dispatch_owners PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_pipeline_dispatch_owners PRIVATE
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS})
target_compile_definitions(bioseq_pipeline_dispatch_owners PRIVATE
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
target_compile_options(bioseq_pipeline_dispatch_owners PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
target_include_directories(bioseq_pipeline_dispatch_owners PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include"
  "${CMAKE_CURRENT_LIST_DIR}/backend"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/safestringlib/include"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  ${Boost_INCLUDE_DIRS}
  ${HWY_INCLUDE_DIRS})
# CMake treats an OBJECT library reached through INTERFACE_LINK_LIBRARIES as
# usage-only. Therefore this PUBLIC edge supplies the pipe BMI to the owner
# dyndep without reinjecting bioseq_modules_nopg objects when the DSO links
# both OBJECT targets directly.
target_link_libraries(bioseq_pipeline_dispatch_owners
  PUBLIC bioseq_modules_nopg
  PRIVATE
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
    hwy::hwy
    ${Boost_LIBRARIES})
if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
  set_target_properties(bioseq_pipeline_dispatch_owners PROPERTIES
    JOB_POOL_COMPILE heavy_compile)
endif()

if(NOT TARGET bioseq_pipeline_dispatch_owners)
  message(FATAL_ERROR
    "Post-erasure pipeline owner OBJECT target was not created")
endif()
get_target_property(_bioseq_pipeline_dispatch_owner_target_type
  bioseq_pipeline_dispatch_owners TYPE)
if(NOT _bioseq_pipeline_dispatch_owner_target_type STREQUAL "OBJECT_LIBRARY")
  message(FATAL_ERROR
    "Post-erasure pipeline owners must use a dedicated OBJECT target")
endif()
get_target_property(_bioseq_pipeline_dispatch_owner_target_sources
  bioseq_pipeline_dispatch_owners SOURCES)
set(_bioseq_pipeline_dispatch_owner_expected_sources
  ${BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS})
list(SORT _bioseq_pipeline_dispatch_owner_target_sources)
list(SORT _bioseq_pipeline_dispatch_owner_expected_sources)
if(NOT "${_bioseq_pipeline_dispatch_owner_target_sources}" STREQUAL
    "${_bioseq_pipeline_dispatch_owner_expected_sources}")
  message(FATAL_ERROR
    "Pipeline owner OBJECT target source attachment is not the active exact "
    "set.\nAttached: ${_bioseq_pipeline_dispatch_owner_target_sources}\n"
    "Expected: ${_bioseq_pipeline_dispatch_owner_expected_sources}")
endif()
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  set(_bioseq_pipeline_dispatch_owner_occurrences 0)
  foreach(_bioseq_pipeline_dispatch_owner_target_source IN LISTS
      _bioseq_pipeline_dispatch_owner_target_sources)
    if(_bioseq_pipeline_dispatch_owner_target_source STREQUAL
        _bioseq_pipeline_dispatch_owner)
      math(EXPR _bioseq_pipeline_dispatch_owner_occurrences
        "${_bioseq_pipeline_dispatch_owner_occurrences} + 1")
    endif()
  endforeach()
  if(_bioseq_pipeline_dispatch_owner IN_LIST
      BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS)
    if(NOT _bioseq_pipeline_dispatch_owner_occurrences EQUAL 1)
      message(FATAL_ERROR
        "Active pipeline owner must occur exactly once in its OBJECT target: "
        "${_bioseq_pipeline_dispatch_owner}")
    endif()
    get_source_file_property(_bioseq_pipeline_dispatch_owner_scan
      "${_bioseq_pipeline_dispatch_owner}" CXX_SCAN_FOR_MODULES)
    if(NOT _bioseq_pipeline_dispatch_owner_scan)
      message(FATAL_ERROR
        "Active pipeline owner must participate in its dedicated CXX "
        "dyndep: ${_bioseq_pipeline_dispatch_owner}")
    endif()
  elseif(NOT _bioseq_pipeline_dispatch_owner_occurrences EQUAL 0)
    message(FATAL_ERROR
      "Inactive pipeline owner was attached to its OBJECT target: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
get_target_property(_bioseq_pipeline_dispatch_owner_manual_dependencies
  bioseq_pipeline_dispatch_owners MANUALLY_ADDED_DEPENDENCIES)
if(_bioseq_pipeline_dispatch_owner_manual_dependencies)
  message(FATAL_ERROR
    "Pipeline owner OBJECT target must not have target-wide generated "
    "dependencies: ${_bioseq_pipeline_dispatch_owner_manual_dependencies}")
endif()
get_target_property(_bioseq_pipeline_dispatch_owner_direct_links
  bioseq_pipeline_dispatch_owners LINK_LIBRARIES)
set(_bioseq_pipeline_dispatch_owner_nopg_links 0)
foreach(_bioseq_pipeline_dispatch_owner_direct_link IN LISTS
    _bioseq_pipeline_dispatch_owner_direct_links)
  if(_bioseq_pipeline_dispatch_owner_direct_link STREQUAL
      "bioseq_modules_nopg")
    math(EXPR _bioseq_pipeline_dispatch_owner_nopg_links
      "${_bioseq_pipeline_dispatch_owner_nopg_links} + 1")
  endif()
endforeach()
if(NOT _bioseq_pipeline_dispatch_owner_nopg_links EQUAL 1)
  message(FATAL_ERROR
    "Pipeline owner OBJECT target must consume bioseq_modules_nopg exactly "
    "once for the public pipe BMI")
endif()
unset(_bioseq_pipeline_dispatch_owner_nopg_links)
unset(_bioseq_pipeline_dispatch_owner_direct_link)
unset(_bioseq_pipeline_dispatch_owner_direct_links)
unset(_bioseq_pipeline_dispatch_owner_manual_dependencies)
unset(_bioseq_pipeline_dispatch_owner_scan)
unset(_bioseq_pipeline_dispatch_owner_occurrences)
unset(_bioseq_pipeline_dispatch_owner_target_source)
unset(_bioseq_pipeline_dispatch_owner)
unset(_bioseq_pipeline_dispatch_owner_expected_sources)
unset(_bioseq_pipeline_dispatch_owner_target_sources)
unset(_bioseq_pipeline_dispatch_owner_target_type)

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_highway_target_graph.cmake")

if(APPLE)
  add_executable(bioseq_gpu_archive_builder
    ${BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS})
  target_sources(bioseq_gpu_archive_builder
    PRIVATE
      FILE_SET archive_modules TYPE CXX_MODULES FILES
        ${BIOSEQ_GPU_ARCHIVE_MODULE_UNITS})
  set_source_files_properties(
    ${BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS}
    PROPERTIES
      CXX_SCAN_FOR_MODULES ON
      COMPILE_OPTIONS "-x;c++")
  target_compile_features(bioseq_gpu_archive_builder PRIVATE cxx_std_26)
  target_link_libraries(bioseq_gpu_archive_builder PRIVATE Vulkan::Headers)
  if(DEFINED PostgreSQL_PKG_LIBRARY_DIR)
    install(TARGETS bioseq_gpu_archive_builder
      RUNTIME DESTINATION "${PostgreSQL_PKG_LIBRARY_DIR}"
      COMPONENT ${PROJECT_NAME})
  endif()
endif()

# ============================================================================
# Tests/integration-tests source ownership. Catch2 support stays outside the
# production module library and is shared by the test executables below.
# ============================================================================

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_test_source_inventory.cmake")

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_postgres_source_graph.cmake")

# The unit target alone compiles the event listener through its TESTS closure.
# Both core Catch executables link the shared indexing fakes; every Domain N
# module is delivered by bioseq_modules_nopg. The integration target adds its
# integration-specific modules below.
include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_test_target_graph.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_sql_codegen.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/postgres_error_safety.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/postgres_error_safety_runtime.cmake")

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_sql_generation_target_graph.cmake")

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_sql_contract_target_graph.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_postgresql_target_graph.cmake")

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_postgres_error_safety_target_graph.cmake")

# bench_framework — generic, algorithm-agnostic benchmark orchestration.
include("${CMAKE_CURRENT_LIST_DIR}/../bench_framework/build.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/io/build.cmake")

# Primary alignment benchmark, built on top of bench_framework.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/alignment_benchmark.cmake")

# End-to-end SQL benchmark, driving PostgreSQL over libpq
# (bioseq.load + mmsearch) against minimap2.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/bench_e2e.cmake")

include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_directory_target_finalization.cmake")
