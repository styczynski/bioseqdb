# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS ${VCPKG_CMAKE_CONFIGURE_OPTIONS} -DSQLGEN_POSTGRES=1)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
option(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST
  "Run the maintainer PREPARE incremental Ninja contract during the build" ON)
option(BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST
  "Run the maintainer SQL negative/provenance fixture suite during the build" ON)
set(BIOSEQ_PG_BUILD_PROFILE_HEADER
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include/bioseq/pg_build_profile.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${BIOSEQ_PG_BUILD_PROFILE_HEADER}")
file(STRINGS "${BIOSEQ_PG_BUILD_PROFILE_HEADER}"
  _bioseq_template_flatten_profile_line
  REGEX "^#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE [a-z0-9_]+$")
list(LENGTH _bioseq_template_flatten_profile_line
  _bioseq_template_flatten_profile_line_count)
if(NOT _bioseq_template_flatten_profile_line_count EQUAL 1)
  message(FATAL_ERROR
    "${BIOSEQ_PG_BUILD_PROFILE_HEADER} must contain exactly one literal "
    "'#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE <semantic-profile>'")
endif()
string(REGEX REPLACE
  "^#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE ([a-z0-9_]+)$" "\\1"
  BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE
  "${_bioseq_template_flatten_profile_line}")
set(BIOSEQ_TEMPLATE_FLATTEN_PROFILES
  dna4_uint64
  all_alphabets_uint64
  all_coordinates
  all_coordinates_adaptive_lengths
  extended_kmer_coordinates
  full_specialization)
list(FIND BIOSEQ_TEMPLATE_FLATTEN_PROFILES
  "${BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE}"
  _bioseq_default_template_flatten_profile_index)
if(_bioseq_default_template_flatten_profile_index EQUAL -1)
  message(FATAL_ERROR
    "unsupported default BIOSEQ_TEMPLATE_FLATTEN_PROFILE "
    "'${BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE}'")
endif()

# Keep the checked-in header literal as the everyday source of truth while
# allowing isolated qualification presets to compile the complete profile in
# a separate build tree. An empty cache entry selects the checked-in default;
# named qualification presets set the same public profile key explicitly.
set(BIOSEQ_TEMPLATE_FLATTEN_PROFILE "" CACHE STRING
  "Template-flatten specialization profile (empty selects header default)")
set_property(CACHE BIOSEQ_TEMPLATE_FLATTEN_PROFILE PROPERTY
  STRINGS "" ${BIOSEQ_TEMPLATE_FLATTEN_PROFILES})
if("${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}" STREQUAL "")
  set(BIOSEQ_TEMPLATE_FLATTEN_PROFILE
    "${BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE}")
  set(_bioseq_template_flatten_profile_source "pg_build_profile.h default")
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

set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64 OFF)
set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES OFF)
set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS OFF)
set(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES OFF)
set(BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION OFF)
set(_bioseq_template_flatten_capability_definitions)
if(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL "dna4_uint64")
  # The baseline profile deliberately enables no cumulative capability macro.
elseif(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL "all_alphabets_uint64")
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64 ON)
elseif(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL "all_coordinates")
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64 ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES ON)
elseif(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL
    "all_coordinates_adaptive_lengths")
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64 ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS ON)
elseif(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL
    "extended_kmer_coordinates")
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64 ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES ON)
elseif(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL "full_specialization")
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64 ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES ON)
  set(BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION ON)
else()
  message(FATAL_ERROR
    "internal unsupported template-flatten profile "
    "'${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}'")
endif()

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
# Generated Highway implementation units intentionally do not include the
# PostgreSQL build-profile header. Publish the resolved semantic profile and
# cumulative capabilities to every translation unit so generated partitions,
# authored sources, and the owner manifest cannot disagree.
add_compile_definitions(
  BIOSEQ_TEMPLATE_FLATTEN_PROFILE=${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}
  ${_bioseq_template_flatten_capability_definitions})
message(STATUS
  "bioseq: BIOSEQ_TEMPLATE_FLATTEN_PROFILE="
  "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE} "
  "(${_bioseq_template_flatten_profile_source}; checked-in default="
  "${BIOSEQ_DEFAULT_TEMPLATE_FLATTEN_PROFILE})")

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp)
set(BIOSEQ_GPU_ARCHIVE_MODULE_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/builder.cppm")
set(BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/gpu/metal_archive/main.cppm")

file(GLOB_RECURSE SRC_MOD_FILES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/**/*.cppm)
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_GPU_ARCHIVE_MODULE_UNITS}
  ${BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS})

# PostgreSQL must publish the digest-derived MoltenVK environment and install
# its shader provider before the process-local Vulkan Runtime singleton is
# constructed.  Make bypasses fail at configure time, including calls hidden
# in implementation include files instantiated by multiple typed TUs.
file(GLOB_RECURSE BIOSEQ_PG_RUNTIME_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/**/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/**/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/**/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/*.hpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/**/*.hpp)
set(BIOSEQ_PG_RUNTIME_OWNER_ALLOWLIST
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/shader_runtime.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/vulkan.cppm)
foreach(_bioseq_pg_source IN LISTS BIOSEQ_PG_RUNTIME_GUARD_SOURCES)
  list(FIND BIOSEQ_PG_RUNTIME_OWNER_ALLOWLIST
    "${_bioseq_pg_source}" _bioseq_pg_runtime_owner_index)
  if(NOT _bioseq_pg_runtime_owner_index EQUAL -1)
    continue()
  endif()
  file(READ "${_bioseq_pg_source}" _bioseq_pg_source_contents)
  string(REGEX MATCH
    "Runtime[ \t\r\n]*::[ \t\r\n]*instance[ \t\r\n]*\\("
    _bioseq_direct_runtime_entry "${_bioseq_pg_source_contents}")
  if(NOT "${_bioseq_direct_runtime_entry}" STREQUAL "")
    message(FATAL_ERROR
      "Direct PostgreSQL Vulkan Runtime entry in ${_bioseq_pg_source}; use ensure_pg_vulkan_runtime_ready()")
  endif()
endforeach()

# Lazy-facade representation-erasure invariant: sequence consumers must not
# reach through the facade into a concrete scanner/range implementation. These files
# are the complete implementation-owner allowlist:
#   * lazy_sequence.cppm owns the canonical facade adapters;
#   * sequence_scanner*.cppm own scanner implementations and PG leases.
# Any new concrete dependency outside those ownership boundaries fails CMake
# configuration, making the allowlist a repository-enforced invariant.
set(BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_sequence_parts/owners.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_sequence_parts/adapters.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/scanner.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/scanner.cppm)

# Named unit fixtures may deliberately construct concrete scanners to exercise
# the representation-erasure boundary.  Keep this list explicit: a new test is
# not implicitly exempt merely because its filename ends in `_test.cpp`.
set(BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/alignment/block_aligner_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/pipeline/pipe_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/batch_sort_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/boundary_parity_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/fragment_producer_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_sequence_index_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/symbol_counts_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/lazy_minimizer_parity_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_index_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_upload_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_segment_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/two_stack_exact_checked_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/two_stack_minimizers_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/lazy_aligner_parity_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/lazy_sequence_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/lazy_pipeline_parity_test.cpp)
file(GLOB_RECURSE BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.hpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.hpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/tests/unit/**/*.cpp)
string(CONCAT _bioseq_lazy_concrete_pattern
  "(\\.scanner[ \t]*\\("
  "|\\.underlying[ \t]*\\("
  "|(^|[^A-Za-z0-9_])lazy_sequence[ \t]*<"
  "|chunked_span_sequence_scanner[ \t]*<)")
foreach(_bioseq_lazy_source IN LISTS BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES)
  list(FIND BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST
    "${_bioseq_lazy_source}" _bioseq_lazy_allowlist_index)
  if(NOT _bioseq_lazy_allowlist_index EQUAL -1)
    continue()
  endif()
  list(FIND BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
    "${_bioseq_lazy_source}" _bioseq_lazy_fixture_index)
  if(NOT _bioseq_lazy_fixture_index EQUAL -1)
    continue()
  endif()
  file(STRINGS "${_bioseq_lazy_source}" _bioseq_lazy_source_lines)
  foreach(_bioseq_lazy_source_line IN LISTS _bioseq_lazy_source_lines)
    set(_bioseq_lazy_code_line "${_bioseq_lazy_source_line}")
    string(FIND "${_bioseq_lazy_code_line}" "//"
      _bioseq_lazy_line_comment_offset)
    if(NOT _bioseq_lazy_line_comment_offset EQUAL -1)
      string(SUBSTRING "${_bioseq_lazy_code_line}" 0
        ${_bioseq_lazy_line_comment_offset} _bioseq_lazy_code_line)
    endif()
    string(STRIP "${_bioseq_lazy_code_line}" _bioseq_lazy_code_line)
    if(_bioseq_lazy_code_line STREQUAL ""
        OR _bioseq_lazy_code_line MATCHES "^(/\\*|\\*|\\*/|\")")
      continue()
    endif()
    if(_bioseq_lazy_code_line MATCHES
        "${_bioseq_lazy_concrete_pattern}")
      message(FATAL_ERROR
        "Lazy-facade representation-erasure violation in ${_bioseq_lazy_source}; "
        "consume lazy_sequence_view or update the documented ownership boundary; "
        "matched code: ${_bioseq_lazy_code_line}")
    endif()
  endforeach()
endforeach()
unset(_bioseq_lazy_concrete_pattern)

# Lazy-facade completion gate. The broad boundary gate above prevents a
# concrete lazy/scanner type from escaping anywhere in backend code; this
# narrower production-owner sweep also rejects the physical-geometry escape
# hatches named by the specification.
set(BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_parts/stage_graph.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_parts/minimizer_execution.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_parts/execution.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_parts/shape_dispatch.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_parts/entry_points.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_parts/segmented.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe_owners_template.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port_parts/results.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port_parts/seeding_chaining.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port_parts/alignment.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port_parts/postprocessing.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port_parts/execution_stats.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_dp.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_certificates.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_minimizers.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/contract.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/upload.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/memory.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/sketch_dispatch.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_materialization.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/batch.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_routes.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/segmented.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/entry_points.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/batch_sort.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/cpu_preparation.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/typed_template.cppm)
file(GLOB_RECURSE BIOSEQ_LAZY_FACADE_INDEX_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.hpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/*.hpp)
list(APPEND BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES
  ${BIOSEQ_LAZY_FACADE_INDEX_GUARD_SOURCES})
list(REMOVE_DUPLICATES BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES)

# Exact forbidden-token exceptions are valid only between balanced named
# sentinels in the owning minimizer modules.  The state machine rejects
# malformed, nested, duplicate, unknown, mismatched, missing, and unclosed
# scopes.  An exact line is then checked against both its source and active
# private resident/admitted scope, so copying an allowed spelling elsewhere is
# still a configure-time failure.
function(bioseq_private_packed_access_scope_names
    _bioseq_source _bioseq_result)
  set(_bioseq_scopes)
  if("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm")
    set(_bioseq_scopes
      bounded_u32_dna4_view
      run_packed_kernel
      collect_minimizers_bounded_nthash_simd_checked)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm")
    set(_bioseq_scopes
      two_stack_simd_iteration_resident_kernel
      decode_packed_dna4_rank_vector
      decode_packed_nt4_rank_vector
      reset_two_stack_nthash
      consume_two_stack_nthash_packed_fragment
      consume_two_stack_nthash_fragment
      collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked
      run_fragmented_impl)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/upload.cppm")
    set(_bioseq_scopes
      byte_upload_range_address_adapter
      vulkan_sketchable_sequence
      sketch_segment_context_units
      compute_segment_descriptor
      make_sketch_segment_window
      admitted_sketch_collection_limit_bp
      sketch_segment_windows
      make_upload_view_from_window
      make_sequence_upload_view)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_routes.cppm")
    set(_bioseq_scopes
      build_gpu_segment_index_on_device_resident_impl)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/segmented.cppm")
    set(_bioseq_scopes
      collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw
      collect_minimizers_vulkan_segment_resident_impl_or_throw)
  endif()
  set(${_bioseq_result} "${_bioseq_scopes}" PARENT_SCOPE)
endfunction()

# Pure transition helper: production turns a nonempty error into FATAL_ERROR,
# while the configure-time self-check below proves every rejection branch.
function(bioseq_private_packed_access_scope_transition
    _bioseq_active _bioseq_seen _bioseq_allowed _bioseq_kind _bioseq_name
    _bioseq_result_active _bioseq_result_seen _bioseq_result_error)
  set(_bioseq_next_active "${_bioseq_active}")
  set(_bioseq_next_seen "${_bioseq_seen}")
  set(_bioseq_error "")
  list(FIND _bioseq_allowed "${_bioseq_name}" _bioseq_allowed_index)
  if(NOT _bioseq_kind STREQUAL "BEGIN"
      AND NOT _bioseq_kind STREQUAL "END")
    set(_bioseq_error "unknown marker kind '${_bioseq_kind}'")
  elseif(_bioseq_allowed_index EQUAL -1)
    set(_bioseq_error "unknown private packed scope '${_bioseq_name}'")
  elseif(_bioseq_kind STREQUAL "BEGIN")
    if(NOT _bioseq_active STREQUAL "")
      set(_bioseq_error
        "nested private packed scope '${_bioseq_name}' inside '${_bioseq_active}'")
    else()
      list(FIND _bioseq_seen "${_bioseq_name}" _bioseq_seen_index)
      if(NOT _bioseq_seen_index EQUAL -1)
        set(_bioseq_error "duplicate private packed scope '${_bioseq_name}'")
      else()
        set(_bioseq_next_active "${_bioseq_name}")
        list(APPEND _bioseq_next_seen "${_bioseq_name}")
      endif()
    endif()
  elseif(_bioseq_active STREQUAL "")
    set(_bioseq_error "unbalanced END for private packed scope '${_bioseq_name}'")
  elseif(NOT "${_bioseq_active}" STREQUAL "${_bioseq_name}")
    set(_bioseq_error
      "mismatched END '${_bioseq_name}' for private packed scope '${_bioseq_active}'")
  else()
    set(_bioseq_next_active "")
  endif()
  set(${_bioseq_result_active} "${_bioseq_next_active}" PARENT_SCOPE)
  set(${_bioseq_result_seen} "${_bioseq_next_seen}" PARENT_SCOPE)
  set(${_bioseq_result_error} "${_bioseq_error}" PARENT_SCOPE)
endfunction()

function(bioseq_private_packed_access_line_allowed
    _bioseq_source _bioseq_scope _bioseq_line _bioseq_result)
  set(_bioseq_allowed FALSE)
  if("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm")
    if("${_bioseq_scope}" STREQUAL "bounded_u32_dna4_view"
        AND ("${_bioseq_line}" STREQUAL
          "return local_view_units * traits::letters_per_unit - local_bp_size;"
        OR "${_bioseq_line}" STREQUAL
          "[[nodiscard]] HWY_INLINE const std::uint32_t* read_cptr("))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "run_packed_kernel"
        AND "${_bioseq_line}" STREQUAL
          "static_cast<std::uint64_t>(view.padding())")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "collect_minimizers_bounded_nthash_simd_checked"
        AND ("${_bioseq_line}" STREQUAL "traits::letters_per_unit;"
          OR "${_bioseq_line}" STREQUAL "sequence.read_cptr(begin));"))
      set(_bioseq_allowed TRUE)
    endif()
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm")
    if("${_bioseq_scope}" STREQUAL
        "two_stack_simd_iteration_resident_kernel"
        AND "${_bioseq_line}" STREQUAL
          "p_sequence_traits::letters_per_unit + local_pos;")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "decode_packed_dna4_rank_vector"
        AND "${_bioseq_line}" STREQUAL
          "static_assert(p_alphabet_traits::letters_per_unit == letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "decode_packed_nt4_rank_vector"
        AND "${_bioseq_line}" STREQUAL
          "static_assert(p_alphabet_traits::letters_per_unit == letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "reset_two_stack_nthash"
        AND "${_bioseq_line}" STREQUAL
          "|| p_alphabet_traits::letters_per_unit")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "consume_two_stack_nthash_packed_fragment"
        AND ("${_bioseq_line}" STREQUAL
            "|| p_alphabet_traits::letters_per_unit"
          OR "${_bioseq_line}" STREQUAL
            "> packed_fragment.size() * p_alphabet_traits::letters_per_unit) {"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "consume_two_stack_nthash_fragment"
        AND "${_bioseq_line}" STREQUAL
          ": fragment.read_cptr(fragment.cbegin());")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked"
        AND ("${_bioseq_line}" STREQUAL "|| traits::letters_per_unit"
          OR "${_bioseq_line}" STREQUAL
            "constexpr std::size_t lpu = traits::letters_per_unit;"
          OR "${_bioseq_line}" STREQUAL
            ": resident.read_cptr(resident.cbegin());"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "run_fragmented_impl"
        AND "${_bioseq_line}" STREQUAL
          "constexpr std::size_t letters_per_unit = traits::letters_per_unit;")
      set(_bioseq_allowed TRUE)
    endif()
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/upload.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_routes.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/segmented.cppm")
    if("${_bioseq_scope}" STREQUAL "byte_upload_range_address_adapter"
        AND "${_bioseq_line}" STREQUAL
          "[[nodiscard]] const std::byte *read_cptr(const iterator &it) const noexcept {")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "vulkan_sketchable_sequence"
        AND ("${_bioseq_line}" STREQUAL "{ seq.padding() };"
          OR "${_bioseq_line}" STREQUAL "{ seq.read_cptr(seq.cbegin()) };"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "sketch_segment_context_units"
        AND "${_bioseq_line}" STREQUAL
          "std::max<std::uint64_t>(1u, p_traits::letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "compute_segment_descriptor"
        AND ("${_bioseq_line}" STREQUAL
          "constexpr std::size_t letters_per_unit = traits::letters_per_unit;"
          OR "${_bioseq_line}" STREQUAL
            "? static_cast<std::uint32_t>(seq.padding())"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "make_sketch_segment_window"
        AND "${_bioseq_line}" STREQUAL
          "const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "admitted_sketch_collection_limit_bp"
        AND "${_bioseq_line}" STREQUAL
          "std::max<std::uint64_t>(1u, p_traits::letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "sketch_segment_windows"
        AND "${_bioseq_line}" STREQUAL
          "const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "make_upload_view_from_window"
        AND "${_bioseq_line}" STREQUAL
          ".letters_per_unit = static_cast<std::uint32_t>(traits::letters_per_unit),")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "make_sequence_upload_view"
        AND ("${_bioseq_line}" STREQUAL
          "seq.byte_size() == 0 ? nullptr : seq.read_cptr(begin);"
          OR "${_bioseq_line}" STREQUAL
            ".tail_padding = static_cast<std::uint32_t>(seq.padding()),"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "build_gpu_segment_index_on_device_resident_impl"
        AND "${_bioseq_line}" STREQUAL
          "const auto* base = seq.read_cptr(begin_iter);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw"
        AND ("${_bioseq_line}" STREQUAL
          "std::max<std::size_t>(1u, traits::letters_per_unit);"
          OR "${_bioseq_line}" STREQUAL
            "const auto* base = seq.read_cptr(begin_iter);"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL
        "collect_minimizers_vulkan_segment_resident_impl_or_throw"
        AND ("${_bioseq_line}" STREQUAL
          "const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);"
          OR "${_bioseq_line}" STREQUAL
            "const auto* base = seq.read_cptr(begin_iter);"))
      set(_bioseq_allowed TRUE)
    endif()
  endif()
  set(${_bioseq_result} ${_bioseq_allowed} PARENT_SCOPE)
endfunction()

# Convert a source file to a CMake-safe list with exactly one element per
# physical line. C++ semicolons, bracket nesting, and trailing preprocessor
# backslashes all affect CMake list parsing, so encode each before introducing
# the newline separators. Decoding happens only after foreach selects one
# encoded element.
function(bioseq_private_packed_access_read_encoded_physical_lines
    _bioseq_source _bioseq_result)
  file(READ "${_bioseq_source}" _bioseq_contents)
  string(REPLACE "\r\n" "\n" _bioseq_contents "${_bioseq_contents}")
  string(REPLACE "\r" "\n" _bioseq_contents "${_bioseq_contents}")
  set(_bioseq_backslash_token
    "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_BACKSLASH_9F2D__")
  set(_bioseq_semicolon_token
    "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_SEMICOLON_9F2D__")
  set(_bioseq_open_bracket_token
    "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_OPEN_BRACKET_9F2D__")
  set(_bioseq_close_bracket_token
    "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_CLOSE_BRACKET_9F2D__")
  foreach(_bioseq_escape_token IN ITEMS
      "${_bioseq_backslash_token}"
      "${_bioseq_semicolon_token}"
      "${_bioseq_open_bracket_token}"
      "${_bioseq_close_bracket_token}")
    string(FIND "${_bioseq_contents}" "${_bioseq_escape_token}"
      _bioseq_escape_collision)
    if(NOT _bioseq_escape_collision EQUAL -1)
      message(FATAL_ERROR
        "private packed access invariant physical-line escape token collision in ${_bioseq_source}")
    endif()
  endforeach()
  string(REPLACE "\\" "${_bioseq_backslash_token}"
    _bioseq_contents "${_bioseq_contents}")
  string(REPLACE ";" "${_bioseq_semicolon_token}"
    _bioseq_contents "${_bioseq_contents}")
  string(REPLACE "[" "${_bioseq_open_bracket_token}"
    _bioseq_contents "${_bioseq_contents}")
  string(REPLACE "]" "${_bioseq_close_bracket_token}"
    _bioseq_contents "${_bioseq_contents}")
  string(REPLACE "\n" ";" _bioseq_lines "${_bioseq_contents}")
  set(${_bioseq_result} "${_bioseq_lines}" PARENT_SCOPE)
endfunction()

function(bioseq_private_packed_access_decode_physical_line
    _bioseq_encoded _bioseq_result)
  set(_bioseq_line "${_bioseq_encoded}")
  string(REPLACE "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_SEMICOLON_9F2D__" ";"
    _bioseq_line "${_bioseq_line}")
  string(REPLACE "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_OPEN_BRACKET_9F2D__" "["
    _bioseq_line "${_bioseq_line}")
  string(REPLACE "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_CLOSE_BRACKET_9F2D__" "]"
    _bioseq_line "${_bioseq_line}")
  string(REPLACE "__BIOSEQ_PRIVATE_PACKED_LINE_ESCAPE_BACKSLASH_9F2D__" "\\"
    _bioseq_line "${_bioseq_line}")
  set(${_bioseq_result} "${_bioseq_line}" PARENT_SCOPE)
endfunction()

# Configure-time negative self-checks for the marker state machine and the
# source+scope+exact-line binding.  These do not touch production files.
set(_bioseq_private_packed_self_allowed alpha beta)
bioseq_private_packed_access_scope_transition(
  "" "" "${_bioseq_private_packed_self_allowed}" BEGIN alpha
  _bioseq_self_active _bioseq_self_seen _bioseq_self_error)
if(NOT _bioseq_self_error STREQUAL "" OR NOT _bioseq_self_active STREQUAL "alpha")
  message(FATAL_ERROR "private packed access invariant private-scope guard positive self-check failed")
endif()
bioseq_private_packed_access_scope_transition(
  "${_bioseq_self_active}" "${_bioseq_self_seen}"
  "${_bioseq_private_packed_self_allowed}" BEGIN beta
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_nested)
bioseq_private_packed_access_scope_transition(
  "${_bioseq_self_active}" "${_bioseq_self_seen}"
  "${_bioseq_private_packed_self_allowed}" END beta
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_mismatch)
bioseq_private_packed_access_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_private_packed_self_allowed}" END alpha
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_unbalanced)
bioseq_private_packed_access_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_private_packed_self_allowed}" BEGIN alpha
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_duplicate)
bioseq_private_packed_access_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_private_packed_self_allowed}" BEGIN gamma
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_unknown)
bioseq_private_packed_access_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_private_packed_self_allowed}" OTHER alpha
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_kind)
if(_bioseq_self_nested STREQUAL "" OR _bioseq_self_mismatch STREQUAL ""
    OR _bioseq_self_unbalanced STREQUAL "" OR _bioseq_self_duplicate STREQUAL ""
    OR _bioseq_self_unknown STREQUAL "" OR _bioseq_self_kind STREQUAL "")
  message(FATAL_ERROR "private packed access invariant private-scope guard negative self-check failed")
endif()
set(_bioseq_private_packed_self_source
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm")
bioseq_private_packed_access_line_allowed(
  "${_bioseq_private_packed_self_source}" bounded_u32_dna4_view
  "return local_view_units * traits::letters_per_unit - local_bp_size;"
  _bioseq_self_exact_allowed)
bioseq_private_packed_access_line_allowed(
  "${_bioseq_private_packed_self_source}" run_packed_kernel
  "return local_view_units * traits::letters_per_unit - local_bp_size;"
  _bioseq_self_wrong_scope_allowed)
bioseq_private_packed_access_line_allowed(
  "${_bioseq_private_packed_self_source}" bounded_u32_dna4_view
  "return traits::letters_per_unit;" _bioseq_self_inexact_allowed)
if(NOT _bioseq_self_exact_allowed OR _bioseq_self_wrong_scope_allowed
    OR _bioseq_self_inexact_allowed)
  message(FATAL_ERROR "private packed access invariant private-scope exact-line self-check failed")
endif()

foreach(_bioseq_facade_source IN LISTS
    BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES)
  list(FIND BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
    "${_bioseq_facade_source}" _bioseq_facade_fixture_index)
  if(NOT _bioseq_facade_fixture_index EQUAL -1)
    continue()
  endif()
  bioseq_private_packed_access_scope_names(
    "${_bioseq_facade_source}" _bioseq_facade_allowed_scopes)
  set(_bioseq_facade_active_scope "")
  set(_bioseq_facade_seen_scopes)
  set(_bioseq_facade_seen_token_hashes)
  set(_bioseq_facade_marker_count 0)
  bioseq_private_packed_access_read_encoded_physical_lines(
    "${_bioseq_facade_source}" _bioseq_facade_encoded_lines)
  foreach(_bioseq_facade_encoded_line IN LISTS
      _bioseq_facade_encoded_lines)
    bioseq_private_packed_access_decode_physical_line(
      "${_bioseq_facade_encoded_line}" _bioseq_facade_source_line)
    string(FIND "${_bioseq_facade_source_line}"
      "BIOSEQ_PRIVATE_PACKED_ACCESS_" _bioseq_facade_marker_offset)
    if(NOT _bioseq_facade_marker_offset EQUAL -1)
      string(STRIP "${_bioseq_facade_source_line}" _bioseq_facade_marker_line)
      if(_bioseq_facade_marker_line MATCHES
          "^//[ \t]*BIOSEQ_PRIVATE_PACKED_ACCESS_(BEGIN|END)[ \t]+([A-Za-z][A-Za-z0-9_]*)[ \t]*$")
        set(_bioseq_facade_marker_kind "${CMAKE_MATCH_1}")
        set(_bioseq_facade_marker_name "${CMAKE_MATCH_2}")
      else()
        message(FATAL_ERROR
          "private packed access invariant malformed private packed scope marker in ${_bioseq_facade_source}: "
          "${_bioseq_facade_marker_line}")
      endif()
      bioseq_private_packed_access_scope_transition(
        "${_bioseq_facade_active_scope}" "${_bioseq_facade_seen_scopes}"
        "${_bioseq_facade_allowed_scopes}"
        "${_bioseq_facade_marker_kind}" "${_bioseq_facade_marker_name}"
        _bioseq_facade_active_scope _bioseq_facade_seen_scopes
        _bioseq_facade_marker_error)
      if(NOT _bioseq_facade_marker_error STREQUAL "")
        message(FATAL_ERROR
          "private packed access invariant ${_bioseq_facade_marker_error} in ${_bioseq_facade_source}")
      endif()
      math(EXPR _bioseq_facade_marker_count
        "${_bioseq_facade_marker_count} + 1")
      continue()
    endif()
    set(_bioseq_facade_code_line "${_bioseq_facade_source_line}")
    string(FIND "${_bioseq_facade_code_line}" "//"
      _bioseq_facade_comment_offset)
    if(NOT _bioseq_facade_comment_offset EQUAL -1)
      string(SUBSTRING "${_bioseq_facade_code_line}" 0
        ${_bioseq_facade_comment_offset} _bioseq_facade_code_line)
    endif()
    string(STRIP "${_bioseq_facade_code_line}" _bioseq_facade_code_line)
    if(_bioseq_facade_code_line STREQUAL ""
        OR _bioseq_facade_code_line MATCHES "^(/\\*|\\*|\\*/|\")")
      continue()
    endif()
    if(_bioseq_facade_code_line MATCHES
        "(scanner_type|(^|[^A-Za-z0-9_])underlying[ \\t]*\\(|\\.scanner[ \\t]*\\(|fragment_storage_units|traits[ \\t]*::[ \\t]*letters_per_unit|sizeof[ \\t]*\\([^)]*\\)[ \\t]*\\*[ \\t]*CHAR_BIT|\\.padding[ \\t]*\\(|read_cptr)")
      bioseq_private_packed_access_line_allowed(
        "${_bioseq_facade_source}" "${_bioseq_facade_active_scope}"
        "${_bioseq_facade_code_line}"
        _bioseq_facade_private_allowed)
      if(NOT _bioseq_facade_private_allowed)
        message(FATAL_ERROR
          "private packed access invariant facade completion dependency in ${_bioseq_facade_source}; "
          "physical scanner/packed access is permitted only in a named private "
          "resident/admitted kernel; matched code: ${_bioseq_facade_code_line}")
      endif()
      string(SHA256 _bioseq_facade_token_hash
        "${_bioseq_facade_active_scope}|${_bioseq_facade_code_line}")
      list(FIND _bioseq_facade_seen_token_hashes
        "${_bioseq_facade_token_hash}" _bioseq_facade_token_hash_index)
      if(NOT _bioseq_facade_token_hash_index EQUAL -1)
        message(FATAL_ERROR
          "private packed access invariant duplicate exact private packed token in scope "
          "${_bioseq_facade_active_scope} in ${_bioseq_facade_source}: "
          "${_bioseq_facade_code_line}")
      endif()
      list(APPEND _bioseq_facade_seen_token_hashes
        "${_bioseq_facade_token_hash}")
    endif()
  endforeach()
  if(NOT _bioseq_facade_active_scope STREQUAL "")
    message(FATAL_ERROR
      "private packed access invariant unclosed private packed scope '${_bioseq_facade_active_scope}' "
      "in ${_bioseq_facade_source}")
  endif()
  list(LENGTH _bioseq_facade_allowed_scopes
    _bioseq_facade_allowed_scope_count)
  math(EXPR _bioseq_facade_expected_marker_count
    "${_bioseq_facade_allowed_scope_count} * 2")
  if(NOT _bioseq_facade_marker_count EQUAL
      _bioseq_facade_expected_marker_count)
    message(FATAL_ERROR
      "private packed access invariant private packed marker count mismatch in ${_bioseq_facade_source}; "
      "expected=${_bioseq_facade_expected_marker_count} "
      "actual=${_bioseq_facade_marker_count}")
  endif()
  foreach(_bioseq_facade_required_scope IN LISTS
      _bioseq_facade_allowed_scopes)
    list(FIND _bioseq_facade_seen_scopes
      "${_bioseq_facade_required_scope}" _bioseq_facade_required_scope_index)
    if(_bioseq_facade_required_scope_index EQUAL -1)
      message(FATAL_ERROR
        "private packed access invariant missing private packed scope '${_bioseq_facade_required_scope}' "
        "in ${_bioseq_facade_source}")
    endif()
  endforeach()
endforeach()
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
# CMake's FindVulkan only materialises Vulkan::glslang when the glslang
# component is requested, so it is listed explicitly. The shader is GLSL and
# does not require DXC, preserving valid MoltenVK and non-Windows configurations.
find_package(Vulkan REQUIRED COMPONENTS glslang)

if(NOT TARGET Vulkan::glslang)
  message(FATAL_ERROR
    "the glslang dependency must provide Vulkan::glslang; rerun vcpkg install glslang")
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
      "glslang did not expose a required static library; rerun vcpkg install glslang")
  endif()
endforeach()

# Benchmark target wiring is included near the end of this file;
# `bioseq_benchmark` consumes `bench_framework`.

# ============================================================================
# Cross-target BMI sharing uses an OBJECT library that owns the domain-neutral
# (NO_POSTGRES) module file set. Consumers link its PUBLIC CXX_MODULES file set
# and compile only their target-private modules.
#
# `testing.cppm` is excluded — it pulls in Catch2 reporter headers; non-test
# binaries do not link Catch2. Test targets add it as a target-private module.
# ============================================================================

# Domain-neutral membership is structural: everything under backend/** and
# instrumentation/** except the PostgreSQL-owned set under backend/pg/**.
# PostgreSQL implementation units are reclassified below after the structural
# glob so named implementations remain ordinary private sources. testing.cppm
# stays target-private because it imports Catch2.
file(GLOB_RECURSE BIOSEQ_NOPG_MODULES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/**/*.cppm)
list(FILTER BIOSEQ_NOPG_MODULES EXCLUDE REGEX "/backend/pg/")
list(FILTER BIOSEQ_NOPG_MODULES EXCLUDE REGEX "/common/testing\.cppm$")
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  ${BIOSEQ_GPU_ARCHIVE_MODULE_UNITS}
  ${BIOSEQ_GPU_ARCHIVE_ORDINARY_UNITS})

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

# Generated scheduler sources must reach the heavy pipeline only through the
# distinct scheduler-owned entries. A compatibility-template call or explicit
# template-id would silently restore one heavy instantiation per leaf.
set(_bioseq_pipeline_dispatch_owner_bypass_source
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/typed_template.cppm")
file(READ "${_bioseq_pipeline_dispatch_owner_bypass_source}"
  _bioseq_pipeline_dispatch_owner_bypass_text)
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
  if(_bioseq_pipeline_dispatch_owner_bypass_text MATCHES
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
    minimap_port_hash_major_owned
    minimap_port_grouped_soa_owned
    minimap_port_fragment_query_owned)
  if(NOT _bioseq_pipeline_dispatch_owner_bypass_text MATCHES
      "${_bioseq_pipeline_dispatch_owned_entry}[ \t\r\n]*\\(")
    message(FATAL_ERROR
      "Generated scheduler input does not call required post-erasure entry "
      "'${_bioseq_pipeline_dispatch_owned_entry}'")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owned_entry)
unset(_bioseq_pipeline_dispatch_owner_bypass_pattern)
unset(_bioseq_pipeline_dispatch_owner_bypass_text)
unset(_bioseq_pipeline_dispatch_owner_bypass_source)

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
file(GLOB_RECURSE BIOSEQ_IO_MODULES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/io/*.cppm")
list(REMOVE_ITEM BIOSEQ_IO_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/io/benchmark/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/io/benchmark/main.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/io/benchmark/common.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/io/benchmark/lazy_expression.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/io/benchmark/readers.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/io/benchmark/writers.cppm")
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
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/logging/formatters.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/common/context.cppm
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/clock.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/concurrency_mode.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/concepts.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/parallelism/pool.cppm)

# The external API client needs Bioseq allocation and worker-pool contracts,
# but not sequence/index/dispatch modules. Keep one canonical provider for this
# small runtime closure and share it with the full Domain-N library.
set(BIOSEQ_NOPG_OWNED_MODULES ${BIOSEQ_NOPG_MODULES})
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

# The generation contract validates the complete annotation -> prepared Clang
# closure -> fixed-target module -> generated binder/registry path, including
# SneakySnake. Families extend explicit registries while target discovery
# remains Highway-owned through HWY_VISIT_TARGETS.
set(BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/highway")

# Probe the unattached scheduler source as raw tokens before constructing any
# typed Highway alignment or minimizer slots. The topology manifest is the
# single alphabet/width/shard authority shared by both generators; neither
# frontend may rediscover or multiply the other's domain.
set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/typed_template.cppm")
set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/template_flatten/dispatch_processor_typed")
cmake_path(IS_PREFIX BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}" NORMALIZE
  _bioseq_template_flatten_root_inside_highway_root)
cmake_path(IS_PREFIX BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${BIOSEQ_HIGHWAY_GENERATED_ROOT}" NORMALIZE
  _bioseq_highway_root_inside_template_flatten_root)
cmake_path(IS_PREFIX BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}" NORMALIZE
  _bioseq_pipeline_owner_root_inside_highway_root)
cmake_path(IS_PREFIX BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT
  "${BIOSEQ_HIGHWAY_GENERATED_ROOT}" NORMALIZE
  _bioseq_highway_root_inside_pipeline_owner_root)
cmake_path(IS_PREFIX BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}" NORMALIZE
  _bioseq_pipeline_owner_root_inside_scheduler_root)
cmake_path(IS_PREFIX BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT
  "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}" NORMALIZE
  _bioseq_scheduler_root_inside_pipeline_owner_root)
if(_bioseq_template_flatten_root_inside_highway_root
    OR _bioseq_highway_root_inside_template_flatten_root
    OR _bioseq_pipeline_owner_root_inside_highway_root
    OR _bioseq_highway_root_inside_pipeline_owner_root
    OR _bioseq_pipeline_owner_root_inside_scheduler_root
    OR _bioseq_scheduler_root_inside_pipeline_owner_root)
  message(FATAL_ERROR
    "Highway, scheduler, and pipeline-owner generated roots must be pairwise "
    "disjoint; "
    "highway='${BIOSEQ_HIGHWAY_GENERATED_ROOT}', "
    "scheduler='${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}', "
    "pipeline_owner='${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}'")
endif()
unset(_bioseq_scheduler_root_inside_pipeline_owner_root)
unset(_bioseq_pipeline_owner_root_inside_scheduler_root)
unset(_bioseq_highway_root_inside_pipeline_owner_root)
unset(_bioseq_pipeline_owner_root_inside_highway_root)
unset(_bioseq_highway_root_inside_template_flatten_root)
unset(_bioseq_template_flatten_root_inside_highway_root)
bioseq_template_flatten_probe(
  NAME dispatch_processor_typed
  SOURCE "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.postgres.scheduler.dispatch_processor
  PROFILE "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}"
  OUT_CONFIGURE_TOPOLOGY_MANIFEST BIOSEQ_TYPED_CONFIGURE_TOPOLOGY
  OUT_TOPOLOGY_DIGEST BIOSEQ_TYPED_TOPOLOGY_DIGEST
  OUT_ENABLED_ORDINALS BIOSEQ_TYPED_ENABLED_ORDINALS
  OUT_ENABLED_SOURCES BIOSEQ_TYPED_PROBED_ENABLED_SOURCES)
bioseq_template_flatten_read_group(
  TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  ROLE identity
  GROUP alphabet
  OUT_LABELS BIOSEQ_TYPED_ALPHABET_LABELS
  OUT_REPLACEMENTS BIOSEQ_TYPED_ALPHABET_REPLACEMENTS
  OUT_ORDINALS BIOSEQ_TYPED_ALPHABET_ORDINALS)
bioseq_template_flatten_read_group(
  TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  ROLE identity
  GROUP width
  OUT_LABELS BIOSEQ_TYPED_WIDTH_LABELS
  OUT_REPLACEMENTS BIOSEQ_TYPED_WIDTH_REPLACEMENTS
  OUT_ORDINALS BIOSEQ_TYPED_WIDTH_ORDINALS)

set(BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS)
set(BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS)
foreach(_bioseq_highway_alignment_alphabet_label
    _bioseq_highway_alignment_alphabet_replacement
    _bioseq_highway_alignment_alphabet_ordinal
    IN ZIP_LISTS
      BIOSEQ_TYPED_ALPHABET_LABELS
      BIOSEQ_TYPED_ALPHABET_REPLACEMENTS
      BIOSEQ_TYPED_ALPHABET_ORDINALS)
  set(_bioseq_highway_alignment_family
    "alignment_${_bioseq_highway_alignment_alphabet_ordinal}")
  list(APPEND BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS
    "ksw2|${_bioseq_highway_alignment_family}|align_ksw2_single_affine|&::bioseq::simd::{target_namespace}::align_ksw2_single_affine<${_bioseq_highway_alignment_alphabet_replacement}>"
    "ksw2|${_bioseq_highway_alignment_family}|align_ksw2_dual_affine|&::bioseq::simd::{target_namespace}::align_ksw2_dual_affine<${_bioseq_highway_alignment_alphabet_replacement}>"
    "ksw2_batched|${_bioseq_highway_alignment_family}|align_ksw2_batched|&::bioseq::simd::{target_namespace}::align_ksw2_batched<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|prepare_block_global_gap_certificate|&::bioseq::simd::{target_namespace}::prepare_block_global_gap_certificate<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|try_block_certified_gap|&::bioseq::simd::{target_namespace}::try_block_certified_gap<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|align_block|&::bioseq::simd::{target_namespace}::align_block<${_bioseq_highway_alignment_alphabet_replacement}>")
  list(APPEND BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS
    "${_bioseq_highway_alignment_family}|align_ksw2_single_affine"
    "${_bioseq_highway_alignment_family}|align_ksw2_dual_affine"
    "${_bioseq_highway_alignment_family}|align_ksw2_batched"
    "${_bioseq_highway_alignment_family}|prepare_block_global_gap_certificate"
    "${_bioseq_highway_alignment_family}|try_block_certified_gap"
    "${_bioseq_highway_alignment_family}|align_block")
endforeach()
unset(_bioseq_highway_alignment_family)
unset(_bioseq_highway_alignment_alphabet_label)
unset(_bioseq_highway_alignment_alphabet_replacement)
unset(_bioseq_highway_alignment_alphabet_ordinal)

# Minimizer target wrappers use the same scheduler-owned alphabet/width axes.
# Each synthetic family is one Cartesian case; the Highway renderer validates
# exact 2 x 4 closure and folds these leaves back into the neutral three-member
# MinimizerKernels table. The renderer also reconciles these records with the
# typed alignment families to emit the root table's neutral alphabet identity;
# no additional CMake alphabet registry participates.
set(BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS)
set(BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS)
foreach(_bioseq_highway_minimizer_alphabet_label
    _bioseq_highway_minimizer_alphabet_replacement
    _bioseq_highway_minimizer_alphabet_ordinal
    IN ZIP_LISTS
      BIOSEQ_TYPED_ALPHABET_LABELS
      BIOSEQ_TYPED_ALPHABET_REPLACEMENTS
      BIOSEQ_TYPED_ALPHABET_ORDINALS)
  foreach(_bioseq_highway_minimizer_width_label
      _bioseq_highway_minimizer_width_replacement
      _bioseq_highway_minimizer_width_ordinal
      IN ZIP_LISTS
        BIOSEQ_TYPED_WIDTH_LABELS
        BIOSEQ_TYPED_WIDTH_REPLACEMENTS
        BIOSEQ_TYPED_WIDTH_ORDINALS)
    if(NOT _bioseq_highway_minimizer_width_replacement
        MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR
        "scheduler topology width replacement must be a positive byte count: "
        "'${_bioseq_highway_minimizer_width_replacement}'")
    endif()
    math(EXPR _bioseq_highway_minimizer_width_minus_one
      "${_bioseq_highway_minimizer_width_replacement} - 1")
    math(EXPR _bioseq_highway_minimizer_width_power_remainder
      "${_bioseq_highway_minimizer_width_replacement} & ${_bioseq_highway_minimizer_width_minus_one}")
    if(_bioseq_highway_minimizer_width_replacement GREATER 8
        OR NOT _bioseq_highway_minimizer_width_power_remainder EQUAL 0)
      message(FATAL_ERROR
        "scheduler topology minimizer width must be a power-of-two byte count "
        "no greater than eight: "
        "'${_bioseq_highway_minimizer_width_replacement}'")
    endif()
    math(EXPR _bioseq_highway_minimizer_width_bits
      "${_bioseq_highway_minimizer_width_replacement} * 8")
    set(_bioseq_highway_minimizer_family
      "minimizer_case_${_bioseq_highway_minimizer_alphabet_ordinal}_${_bioseq_highway_minimizer_width_replacement}")
    set(_bioseq_highway_minimizer_template_arguments
      "${_bioseq_highway_minimizer_alphabet_replacement},::std::uint${_bioseq_highway_minimizer_width_bits}_t")
    list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS
      "minimizer_minimap2_reference|${_bioseq_highway_minimizer_family}|minimap2_reference|&::bioseq::simd::{target_namespace}::minimap2_reference_case<${_bioseq_highway_minimizer_template_arguments}>"
      "minimizer_two_stack_nthash|${_bioseq_highway_minimizer_family}|two_stack_nthash|&::bioseq::simd::{target_namespace}::two_stack_nthash_case<${_bioseq_highway_minimizer_template_arguments}>"
      "minimizer_bounded_nthash|${_bioseq_highway_minimizer_family}|bounded_nthash|&::bioseq::simd::{target_namespace}::bounded_nthash_case<${_bioseq_highway_minimizer_template_arguments}>")
    list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS
      "${_bioseq_highway_minimizer_family}|minimap2_reference"
      "${_bioseq_highway_minimizer_family}|two_stack_nthash"
      "${_bioseq_highway_minimizer_family}|bounded_nthash")
  endforeach()
endforeach()
unset(_bioseq_highway_minimizer_family)
unset(_bioseq_highway_minimizer_template_arguments)
unset(_bioseq_highway_minimizer_width_bits)
unset(_bioseq_highway_minimizer_width_power_remainder)
unset(_bioseq_highway_minimizer_width_minus_one)
unset(_bioseq_highway_minimizer_width_label)
unset(_bioseq_highway_minimizer_width_replacement)
unset(_bioseq_highway_minimizer_width_ordinal)
unset(_bioseq_highway_minimizer_alphabet_label)
unset(_bioseq_highway_minimizer_alphabet_replacement)
unset(_bioseq_highway_minimizer_alphabet_ordinal)

# Target-private semantic probes are four ordinary implementation
# units of the one already-generated static-target primary. They are generator
# templates, never authored/final module providers or production slot owners.
set(BIOSEQ_HIGHWAY_UNIT_PROBE_RECORDS
  "hashers|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_test_probes/hashers.cppm.in"
  "sequence_ops|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_test_probes/sequence_ops.cppm.in"
  "simd_utils|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_test_probes/simd_utils.cppm.in"
  "two_stack|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_test_probes/two_stack.cppm.in")
# One bounded ordinary owner per admitted result representation prevents every
# coarse input-width wrapper from instantiating the complete result-layout
# switch. Baseline k-mer profiles have 7 owners / 14 typed instantiations;
# extended-k-mer profiles add K7/K27 identities for exactly 13 owners / 24
# instantiations per target.
set(BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
  "minimap2_reference_l15_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<15u,15u>|u32,u64"
  "minimap2_reference_l28_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<28u,15u>|u32,u64"
  "minimap2_reference_l33_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<33u,15u>|u32,u64"
  "minimap2_reference_l63_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<63u,15u>|u32,u64"
  "bounded_nthash_l15_k15|bounded_nthash|minimizer_bounded_nthash|::bioseq::minimizers::minimizer_representation<15u,15u>|u32,u64"
  "bounded_nthash_l28_k15|bounded_nthash|minimizer_bounded_nthash|::bioseq::minimizers::minimizer_representation<28u,15u>|u32,u64"
  "bounded_nthash_l63_k15|bounded_nthash|minimizer_bounded_nthash|::bioseq::minimizers::minimizer_representation<63u,15u>|u32,u64")
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
  list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
    "minimap2_reference_l7_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<7u,7u>|u32,u64"
    "minimap2_reference_l17_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<17u,7u>|u32,u64"
    "minimap2_reference_l49_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<49u,7u>|u32,u64"
    "minimap2_reference_l63_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<63u,7u>|u32,u64"
    "minimap2_reference_l27_k27|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<27u,27u>|u64"
    "minimap2_reference_l63_k27|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<63u,27u>|u64")
endif()

bioseq_add_highway_codegen(
  REQUIRE_COMPLETE_TABLE
  TARGET bioseq_highway_target_modules
  POLICY_TARGET bioseq_highway_target_policy
  OUTPUT_ROOT "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
  CONTRACT_MODULE bioseq.highway.dispatch_contract
  DISPATCH_MODULE bioseq.postgres.scheduler.dispatch_processor
  HIGHWAY_VERSION 1.3.0
  # Block Aligner requires at least eight lanes. EMU128 remains the portable
  # fixed-width fallback; scalar and sizeless targets are outside this table.
  POLICY_DEFINITIONS
    HWY_COMPILE_ALL_ATTAINABLE=1
    "HWY_DISABLED_TARGETS=(HWY_SCALAR|HWY_ALL_SVE|HWY_RVV)"
  DOMAIN_TOPOLOGY_MANIFESTS
    "typed_scheduler=${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_dispatch_contract.cppm"
  PCM_ROOT "${BIOSEQ_HIGHWAY_GENERATED_ROOT}/pcm"
  SOURCE_RECORDS
    "simd_utils|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_utils.cppm|bioseq.highway.sources.simd_utils|simd_utils|simd_utils|"
    "vector_stream_operations|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_vector_stream_operations.cppm|bioseq.highway.sources.vector_stream_operations|vector_stream_operations|vector_stream|"
    "vector_stream|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_vector_stream.cppm|bioseq.highway.sources.vector_stream|vector_stream|vector_stream|simd_utils,vector_stream_operations"
    "sequence_ops|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/highway_sequence_ops.cppm|bioseq.highway.sources.sequence_ops|sequence_ops|sequence_ops|simd_utils,vector_stream"
    "hasher_base|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_base_kernel.cppm|bioseq.highway.sources.hasher_base|hasher_base|hasher_base|sequence_ops,simd_utils"
    "hasher_lexic|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_lexic_kernel.cppm|bioseq.highway.sources.hasher_lexic|hasher_lexic|hasher_lexic|hasher_base,simd_utils"
    "hasher_minimap_int|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_minimap_int_kernel.cppm|bioseq.highway.sources.hasher_minimap_int|hasher_minimap_int|hasher_minimap_int|hasher_base,hasher_lexic,simd_utils"
    "minimizer_minimap2_reference|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimap2_reference_kernel.cppm|bioseq.highway.sources.minimizer_minimap2_reference|minimizer_minimap2_reference|minimizers|sequence_ops,hasher_base,hasher_minimap_int,simd_utils,vector_stream"
    "minimizer_two_stack_nthash|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm|bioseq.highway.sources.minimizer_two_stack_nthash|minimizer_two_stack_nthash|minimizers|sequence_ops,hasher_base,hasher_lexic,hasher_minimap_int,simd_utils,vector_stream"
    "minimizer_bounded_nthash|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm|bioseq.highway.sources.minimizer_bounded_nthash|minimizer_bounded_nthash|minimizers|sequence_ops,hasher_base,simd_utils,vector_stream"
    "alignment_utils|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/utils_kernel.cppm|bioseq.highway.sources.alignment_utils|alignment_utils|alignment|"
    "ksw2_primitives|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_primitives.cppm|bioseq.highway.sources.ksw2_primitives|ksw2_primitives|alignment|"
    "ksw2|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_kernel.cppm|bioseq.highway.sources.ksw2|ksw2|alignment|alignment_utils,ksw2_primitives"
    "block_aligner_dp|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_dp.cppm|bioseq.highway.sources.block_aligner_dp|block_aligner_dp|alignment|alignment_utils,simd_utils"
    "block_aligner_traceback|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_traceback.cppm|bioseq.highway.sources.block_aligner_traceback|block_aligner_traceback|alignment|block_aligner_dp"
    "block_aligner_certificates|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_certificates.cppm|bioseq.highway.sources.block_aligner_certificates|block_aligner_certificates|alignment|alignment_utils,block_aligner_dp"
    "block_aligner|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm|bioseq.highway.sources.block_aligner|block_aligner|alignment|alignment_utils,block_aligner_certificates,block_aligner_dp,block_aligner_traceback"
    "ksw2_batched|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm|bioseq.highway.sources.ksw2_batched|ksw2_batched|alignment|ksw2,alignment_utils"
    "prefilter|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/prefilter/sneaky_snake/kernel.cppm|bioseq.highway.sources.sneaky_snake|prefilter|common|simd_utils"
    "chaining|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_kernel.cppm|bioseq.highway.sources.chaining|chaining|chaining|simd_utils"
    "indexing|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/highway_indexing_kernels.cppm|bioseq.highway.sources.indexing|indexing|indexing|"
  SLOT_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS}
    ${BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS}
    "prefilter|common|sneaky_snake_simd_batch|&bioseq::prefilter::{target_namespace}::sneaky_snake_simd_batch"
    "chaining|chaining|chain_anchors_simd|&::bioseq::simd::{target_namespace}::chain_anchors_simd"
  REQUIRED_SLOT_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS}
    ${BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS}
    "common|sneaky_snake_simd_batch"
    "chaining|chain_anchors_simd"
  UNIT_PROBE_RECORDS ${BIOSEQ_HIGHWAY_UNIT_PROBE_RECORDS}
  MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS}
  OUTPUT_MODULES BIOSEQ_HIGHWAY_GENERATED_MODULES
  OUTPUT_PRIMARY_MODULES BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES
  OUTPUT_TARGET_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS
  OUTPUT_DISPATCH_REGISTRY BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY
  OUTPUT_DISPATCH_INVENTORY BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY
  OUTPUT_TEST_PRIMARY_MODULE BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE
  OUTPUT_TEST_IMPLEMENTATIONS BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS
  OUTPUT_TEST_SUPPORT BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT
  OUTPUT_UNIT_PROBE_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS
  OUTPUT_MINIMIZER_REPRESENTATION_INSTANCE_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_MINIMIZER_REPRESENTATION_INSTANCE_IMPLEMENTATIONS
  OUTPUT_SOURCE_INPUTS BIOSEQ_HIGHWAY_REGISTERED_SOURCE_INPUTS
  OUTPUT_TARGET_RECORDS BIOSEQ_HIGHWAY_COMPILED_TARGET_RECORDS
  OUTPUT_COMPILED_MASK BIOSEQ_HIGHWAY_COMPILED_TARGET_MASK)

list(LENGTH BIOSEQ_HIGHWAY_UNIT_PROBE_RECORDS
  _bioseq_highway_unit_probe_record_count)
list(LENGTH BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS
  _bioseq_highway_unit_probe_output_count)
list(LENGTH BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
  _bioseq_highway_representation_owner_count)
list(LENGTH BIOSEQ_HIGHWAY_COMPILED_TARGET_RECORDS
  _bioseq_highway_compiled_target_count)
list(LENGTH BIOSEQ_HIGHWAY_GENERATED_MINIMIZER_REPRESENTATION_INSTANCE_IMPLEMENTATIONS
  _bioseq_highway_representation_output_count)
math(EXPR _bioseq_highway_expected_representation_output_count
  "${_bioseq_highway_representation_owner_count} * ${_bioseq_highway_compiled_target_count}")
if(NOT _bioseq_highway_unit_probe_record_count EQUAL 4
    OR NOT _bioseq_highway_unit_probe_output_count EQUAL 4)
  message(FATAL_ERROR
    "Fixed-target generation requires exactly four unit validation owners/outputs")
endif()
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
  set(_bioseq_highway_expected_representation_owner_count 13)
else()
  set(_bioseq_highway_expected_representation_owner_count 7)
endif()
if(NOT _bioseq_highway_representation_owner_count EQUAL
      _bioseq_highway_expected_representation_owner_count
    OR NOT _bioseq_highway_representation_output_count EQUAL
      _bioseq_highway_expected_representation_output_count)
  message(FATAL_ERROR
    "Minimizer representation owner/output closure diverged: "
    "owners=${_bioseq_highway_representation_owner_count}, "
    "outputs=${_bioseq_highway_representation_output_count}, "
    "targets=${_bioseq_highway_compiled_target_count}")
endif()
set(_bioseq_highway_probe_outputs
  ${BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS})
set(_bioseq_highway_unique_probe_outputs ${_bioseq_highway_probe_outputs})
list(REMOVE_DUPLICATES _bioseq_highway_unique_probe_outputs)
list(LENGTH _bioseq_highway_probe_outputs _bioseq_highway_probe_output_count)
list(LENGTH _bioseq_highway_unique_probe_outputs
  _bioseq_highway_unique_probe_output_count)
if(NOT _bioseq_highway_probe_output_count EQUAL
    _bioseq_highway_unique_probe_output_count)
  message(FATAL_ERROR "Highway validation outputs are not disjoint")
endif()
foreach(_bioseq_highway_unit_probe
    IN LISTS BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS)
  if(_bioseq_highway_unit_probe IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS
      OR _bioseq_highway_unit_probe IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS
      OR _bioseq_highway_unit_probe STREQUAL
        BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY
      OR _bioseq_highway_unit_probe STREQUAL
        BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY)
    message(FATAL_ERROR
      "unit probe escaped its dedicated fixed-target output: "
      "${_bioseq_highway_unit_probe}")
  endif()
endforeach()
unset(_bioseq_highway_unit_probe)
unset(_bioseq_highway_probe_outputs)
unset(_bioseq_highway_unique_probe_outputs)
unset(_bioseq_highway_probe_output_count)
unset(_bioseq_highway_unique_probe_output_count)
unset(_bioseq_highway_expected_representation_owner_count)
unset(_bioseq_highway_expected_representation_output_count)
unset(_bioseq_highway_representation_output_count)
unset(_bioseq_highway_compiled_target_count)
unset(_bioseq_highway_representation_owner_count)
unset(_bioseq_highway_unit_probe_output_count)
unset(_bioseq_highway_unit_probe_record_count)

# CMake requires every named module partition provider in a CXX_MODULES file
# set. Keep binders and per-owner instantiation units as ordinary private
# sources, and attach only generated partitions alongside their primaries.
set(BIOSEQ_HIGHWAY_GENERATED_TARGET_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TARGET_ORDINARY_IMPLEMENTATIONS
  INCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS
  EXCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS
  INCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS
  EXCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")

# The compiler-selected fixed target has one reusable owner. Production,
# unit tests, and benchmarks consume the same BMI/object closure by linking
# these targets instead of compiling the generated files independently.
set(BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES
  ${BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES})
list(REMOVE_ITEM BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES
  "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}")
set(BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_ORDINARY_IMPLEMENTATIONS})
list(REMOVE_ITEM BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS})
set(BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS})
list(REMOVE_ITEM BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS})

add_library(bioseq_highway_fixed_target STATIC)
set_target_properties(bioseq_highway_fixed_target PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_highway_fixed_target
  PRIVATE ${BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS}
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES
      "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}"
      ${BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS})
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_link_libraries(bioseq_highway_fixed_target PUBLIC
  bioseq_modules_nopg
  bioseq_highway_target_policy)
add_dependencies(
  bioseq_highway_fixed_target bioseq_highway_target_modules)

add_library(bioseq_highway_test_support STATIC)
set_target_properties(bioseq_highway_test_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_highway_test_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES "${BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT}")
target_link_libraries(bioseq_highway_test_support PUBLIC
  bioseq_highway_fixed_target)
add_dependencies(
  bioseq_highway_test_support bioseq_highway_target_modules)

if(NOT BIOSEQ_HIGHWAY_REGISTERED_SOURCE_INPUTS STREQUAL
    BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES)
  message(FATAL_ERROR
    "Highway template ownership and final-provider exclusion diverged")
endif()

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

find_package(Catch2 3 REQUIRED)

# Bench-prefilter correctness metrics are also unit-tested. Keep their named
# module in one reusable owner instead of recompiling it in the benchmark and
# unit-test targets.
add_library(bioseq_bench_prefilter_metrics STATIC)
set_target_properties(bioseq_bench_prefilter_metrics PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_bench_prefilter_metrics
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/.."
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/../bench/prefilter_pipeline_metrics.cppm")

# Indexing storage fakes and the libpq integration client each have one reusable
# test-support owner. Test executables link these targets instead of recompiling
# the same providers in every consumer.
add_library(bioseq_testing_support OBJECT)
set_target_properties(bioseq_testing_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_testing_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/tests/support/indexing_storage_fakes.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/test_support.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/test_support.cppm")
target_link_libraries(bioseq_testing_support
  PUBLIC
    bioseq_modules_nopg
    bioseq_highway_test_support
    libassert::assert
  PRIVATE
    Catch2::Catch2)

add_library(bioseq_integration_test_support STATIC)
set_target_properties(bioseq_integration_test_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_integration_test_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/tests/integration/support/index.cppm")
target_link_libraries(bioseq_integration_test_support PUBLIC
  fmt
  libassert::assert
  PostgreSQL::PostgreSQL)

# Catch source ownership is explicit. The repository-level ownership audit
# discovers *_test.cpp only to reject omissions or duplicate owners; discovery
# never feeds a target source list.
set(BIOSEQ_UNIT_TEST_SUPPORT_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/tests/support/test_event_listener.cpp")
set(BIOSEQ_UNIT_TEST_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/alignment/block_aligner_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/alignment/edit_distance_accounting_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/chaining/simd_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/chaining/vulkan_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/common/logging_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/gpu/vulkan_shader_cache_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/boundary_parity_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/compact_hash32_direct_index_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/gpu_minimizer_index_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/index_chunk_io_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/merge_runs_multicursor_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_fragment_compact_hash32_adapter_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/generation_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/fragment_producer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/grouped_soa_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_hashmap/compact_direct_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/minimizer_sequence_index_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/persisted_minimizer_image_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/storage_concept_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/indexing/symbol_counts_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/io/file_kind_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/io/identifier_parser/formats_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/io/identifier_parser/identifier_parser_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/highway_minimizer_sink_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/kmers_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/lazy_minimizer_parity_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/minimizer_collector_admission_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/nt4_minimizer_parity_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/two_stack_exact_checked_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/two_stack_exact_continuation_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/two_stack_exact_output_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/two_stack_minimizers_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_memory_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_batch_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_parity_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_index_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_upload_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_segment_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/parallelism/pool_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/pipeline/pipe_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/prefilter/gatekeeper_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/prefilter/sneaky_snake_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/seeding/vulkan_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/sequences/edit_distance_threshold_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/sequences/packed_sequence_split_units_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/sequences/packed_sequence_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/sequences/rank_pack_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/sequences/sequence_generator_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/simd/highway_dispatch_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/simd/highway_fixed_target_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/simd/sorting_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/simd/vector_stream_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/batch_sort_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/layout_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/lazy_aligner_parity_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/lazy_pipeline_parity_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/unit/storage/lazy_sequence_test.cpp")

set(BIOSEQ_INTEGRATION_TEST_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/admin_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/btree_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/chunked_storage_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/e2e_smoke_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/explain_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/gpu_mmsearch_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/index_admin_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/index_invariance_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/index_mixed_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/index_table_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/insert_hook_multifragment_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/io/reader_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/join_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/large_table_alignment_smoke_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/lazy_sequence_expression_property_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/lazy_sequence_slicing_acceptance_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/lazy_sequence_slicing_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/load_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/minimizers_per_sequence_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/mmsearch_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/reindex_scanner_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/row_mask_write_state_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/sample_table_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/scalar_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/sequence_basic_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/sequence_fragment_storage_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/sequence_typmod_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/source_domain_lifetime_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/sql_boundary_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/sql_row_codec_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/storage_buckets_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/substring_property_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/symbol_count_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/typmod_coalesce_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/integration/wire_test.cpp")

list(REMOVE_ITEM SRC_FILES ${BIOSEQ_UNIT_TEST_SOURCES})
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_UNIT_TEST_SOURCES})

# Remove modules owned by the shared bioseq_modules_nopg OBJECT library from
# postgresql_bioseq's private module set. The `SHARED_MODULE_LIBS
# bioseq_modules_nopg` argument below supplies the PUBLIC BMIs, so the
# domain-neutral module set has one build owner.
#
# The shared BMIs are compiled with private NO_POSTGRES=1. The domain-neutral
# modules that branch on NO_POSTGRES therefore use their standalone contracts:
#
#   - sequences/simd_implementation.cppm  ::stdc_compare contract-violation paths use
#     DEBUG_ASSERT. Well-formed callers exclude mixed-alphabet values and
#     cross-storage-width non-bit-castable pairs.
#   - common/mlogging.cppm remains PG-header-free. PG-target logging is
#     delivered through the sink registered by _PG_init(); without a registered
#     sink it falls back to BIOSEQ_LOG_FILE or
#     /tmp/bioseq_loglog.log.
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_NOPG_MODULES})

# The SQL compile-contract provider owns the smallest reusable PostgreSQL
# module closure needed by generated catalog handles, row codecs, and GUC
# facets. Both the extension and the dedicated contract OBJECT target import
# these PUBLIC BMIs; the contract objects themselves never enter the DSO.
set(BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/configuration/aligner.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/configuration/core.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/configuration/scheduler.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/configuration/storage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/column/metadata/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/datums.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/runtime_diagnostics.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_catalog_entities.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/aggregate.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/guc_storage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/row_codec.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/vector.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/io/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/manager/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/sequences/sample_table/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/table/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/vectorization/vector_types.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS})

# Compiler-owned SQL module-reachability fixtures. They are removed from
# the production source globs here and compiled only by the dedicated OBJECT
# target declared after generated provider paths are known.
set(BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/catalog_entity_handle_producer_test.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/generated_guc_producer_test.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/sql_boundary_template_producer_test.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/result_materialization_test.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/sql_entity_handles_test.cppm")
set(BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/catalog_entity_handle_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/generated_guc_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/sql_boundary_template_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tests/contract/postgres/sql/active_generated_guc_consumer_test.cpp")
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})
list(REMOVE_ITEM SRC_FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})
list(REMOVE_DUPLICATES SRC_MOD_FILES)
list(REMOVE_DUPLICATES SRC_FILES)

# External CustomScan (backend/pg/external/scan.cppm), its row writer
# (row_writer.cppm), and provider-error translation (errors.cppm) import the
# external provider stack, so they compile into the extension only when the
# external API clients are enabled. The provider-free relation and path-builder
# modules stay unconditional. The option is declared here (before
# SRC_MOD_FILES is consumed by the postgresql_bioseq target); the later
# add_subdirectory(external) reuses the same cached value.
option(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
  "Configure generated external API clients and their integration tests" OFF)
if(NOT BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  list(FILTER SRC_MOD_FILES EXCLUDE
    REGEX "/backend/pg/external/(errors|row_writer|scan)\\.cppm$")
endif()

# GCC heavy dispatch owners inherit the production preset's -O3 codegen.
# Scheduling and forced-template-inlining controls bound compiler resources
# without lowering optimization or changing runtime dispatch.
set(BIOSEQ_GCC_TYPED_DISPATCH_JOBS "1" CACHE STRING
  "Ninja compile-pool width for GCC 16.1 AWS typed dispatch module units. \
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

# Materialize only the profile-enabled scheduler coordinates. The canonical
# body remains an unattached raw-token input; dependency BMIs use the authored
# primary as their compilation-signature donor.
set(_bioseq_typed_sql_support_profile production)
if(BIOSEQ_TEST_INDEXES)
  set(_bioseq_typed_sql_support_profile test_indexes)
endif()
set(BIOSEQ_TYPED_SQL_SUPPORT_STAMP
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/profiles/${_bioseq_typed_sql_support_profile}/support.stamp")
unset(_bioseq_typed_sql_support_profile)
bioseq_add_template_flatten_codegen(
  NAME dispatch_processor_typed
  SOURCE "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.postgres.scheduler.dispatch_processor
  EMISSION_POLICY selector_api
  CONFIGURE_TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  EXPECTED_TOPOLOGY_DIGEST "${BIOSEQ_TYPED_TOPOLOGY_DIGEST}"
  ENABLED_ORDINALS ${BIOSEQ_TYPED_ENABLED_ORDINALS}
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/index.cppm"
  PCM_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}/pcm"
  PREPARE_DEPENDS "${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}"
  OUT_API_PARTITION BIOSEQ_TYPED_API_PARTITION
  OUT_SELECTOR_PARTITION BIOSEQ_TYPED_SELECTOR_PARTITION
  OUT_IMPLEMENTATION_UNITS BIOSEQ_TYPED_IMPLEMENTATION_UNITS
  OUT_GENERATION_TARGET BIOSEQ_TYPED_GENERATION_TARGET
  OUT_ARTIFACT_MANIFEST BIOSEQ_TYPED_ARTIFACT_MANIFEST)

if(NOT "${BIOSEQ_TYPED_PROBED_ENABLED_SOURCES}" STREQUAL
    "${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}")
  message(FATAL_ERROR
    "Template-flatten probe and generator disagree on enabled scheduler units")
endif()
if(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL "dna4_uint64")
  set(_bioseq_typed_expected_count 7)
elseif(BIOSEQ_TEMPLATE_FLATTEN_PROFILE STREQUAL "all_alphabets_uint64")
  set(_bioseq_typed_expected_count 14)
else()
  set(_bioseq_typed_expected_count 56)
endif()
list(LENGTH BIOSEQ_TYPED_IMPLEMENTATION_UNITS
  BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT)
if(NOT BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT EQUAL
    _bioseq_typed_expected_count)
  message(FATAL_ERROR
    "Template-flatten profile ${BIOSEQ_TEMPLATE_FLATTEN_PROFILE} requires "
    "${_bioseq_typed_expected_count} scheduler units, generated "
    "${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT}")
endif()
unset(_bioseq_typed_expected_count)

set(BIOSEQ_ALIGNMENT_HOOK_PRIVATE_PARTITIONS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_contract.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_core_state.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_core_lookup.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_core_unlink.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_core_retirement.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_core_stack.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_binding.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_run.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_callbacks.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_hook_publish.cppm")

set(BIOSEQ_SUPERVISOR_PRIVATE_PARTITIONS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/persist_queue_publish.cppm")

set(BIOSEQ_DISPATCH_BUFFER_IMPLEMENTATION_PARTITIONS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/configuration.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/join_preparation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/hydration.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/join_execution.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/scan_execution.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/index_parts/output.cppm")

set(BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/lifecycle.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_operation_stats.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/alignment_stats.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/index_admin.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/minimizer_diagnostics.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/scheduler_diagnostics.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/sequence_source_contract.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/sql_frontdoor.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor/implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer/api_implementation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/runtime.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/highway_binding.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/persist.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/persist_stage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/persist_stats.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/persist_worker.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/shader_runtime.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor/direct_minimizer_materialization.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/external/standard_library_link_support.cppm")
list(APPEND BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
  ${BIOSEQ_DISPATCH_BUFFER_IMPLEMENTATION_PARTITIONS}
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/sorted_runs/postgres_error.cppm")
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  if(_bioseq_pipeline_dispatch_owner IN_LIST
      BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS)
    message(FATAL_ERROR
      "Pipeline owner leaked into the PostgreSQL ordinary-unit registry: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner)
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES}
  ${BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS})
list(APPEND SRC_MOD_FILES
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}")
list(REMOVE_DUPLICATES SRC_MOD_FILES)
list(APPEND SRC_FILES
  ${BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS}
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS})
foreach(_bioseq_alignment_hook_partition
    IN LISTS BIOSEQ_ALIGNMENT_HOOK_PRIVATE_PARTITIONS)
  set(_bioseq_alignment_hook_module_count 0)
  foreach(_bioseq_module_source IN LISTS SRC_MOD_FILES)
    if(_bioseq_module_source STREQUAL _bioseq_alignment_hook_partition)
      math(EXPR _bioseq_alignment_hook_module_count
        "${_bioseq_alignment_hook_module_count} + 1")
    endif()
  endforeach()
  if(NOT _bioseq_alignment_hook_module_count EQUAL 1)
    message(FATAL_ERROR
      "Alignment-hook private partition must occur exactly once in "
      "SRC_MOD_FILES: '${_bioseq_alignment_hook_partition}' occurs "
      "${_bioseq_alignment_hook_module_count} times")
  endif()
  if(_bioseq_alignment_hook_partition IN_LIST SRC_FILES
      OR _bioseq_alignment_hook_partition IN_LIST
        BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
      OR _bioseq_alignment_hook_partition IN_LIST
        BIOSEQ_TYPED_IMPLEMENTATION_UNITS
      OR _bioseq_alignment_hook_partition IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_MODULES)
    message(FATAL_ERROR
      "Alignment-hook private partition must be target-owned only through "
      "the CXX_MODULES file set: '${_bioseq_alignment_hook_partition}'")
  endif()
endforeach()
unset(_bioseq_alignment_hook_module_count)
unset(_bioseq_module_source)
unset(_bioseq_alignment_hook_partition)
foreach(_bioseq_supervisor_partition
    IN LISTS BIOSEQ_SUPERVISOR_PRIVATE_PARTITIONS)
  set(_bioseq_supervisor_module_count 0)
  foreach(_bioseq_module_source IN LISTS SRC_MOD_FILES)
    if(_bioseq_module_source STREQUAL _bioseq_supervisor_partition)
      math(EXPR _bioseq_supervisor_module_count
        "${_bioseq_supervisor_module_count} + 1")
    endif()
  endforeach()
  if(NOT _bioseq_supervisor_module_count EQUAL 1)
    message(FATAL_ERROR
      "Supervisor private partition must occur exactly once in "
      "SRC_MOD_FILES: '${_bioseq_supervisor_partition}' occurs "
      "${_bioseq_supervisor_module_count} times")
  endif()
  if(_bioseq_supervisor_partition IN_LIST SRC_FILES
      OR _bioseq_supervisor_partition IN_LIST
        BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
      OR _bioseq_supervisor_partition IN_LIST
        BIOSEQ_TYPED_IMPLEMENTATION_UNITS
      OR _bioseq_supervisor_partition IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_MODULES)
    message(FATAL_ERROR
      "Supervisor private partition must be target-owned only through "
      "the CXX_MODULES file set: '${_bioseq_supervisor_partition}'")
  endif()
endforeach()
unset(_bioseq_supervisor_module_count)
unset(_bioseq_module_source)
unset(_bioseq_supervisor_partition)
set_source_files_properties(
  ${BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS}
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
set_source_files_properties(
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/external/standard_library_link_support.cppm"
  PROPERTIES CXX_SCAN_FOR_MODULES OFF)
set_source_files_properties(
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
  PROPERTIES GENERATED TRUE)
message(STATUS
  "bioseq: enabled ${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT} "
  "typed dispatch processor module implementation units")

if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  set(BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS
    ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
    ${BIOSEQ_PIPELINE_DISPATCH_OWNER_IMPLEMENTATION_UNITS})
  if(NOT BIOSEQ_GCC_TEMPLATE_FORCE_INLINE)
    set_property(SOURCE ${BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS}
      APPEND PROPERTY COMPILE_DEFINITIONS
        BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE=0)
  endif()
  if(BIOSEQ_GCC_TYPED_DISPATCH_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
      gcc_typed_dispatch_compile=${BIOSEQ_GCC_TYPED_DISPATCH_JOBS})
    set_source_files_properties(
      ${BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS}
      PROPERTIES JOB_POOL_COMPILE gcc_typed_dispatch_compile)
  endif()
  message(STATUS
    "bioseq: GCC typed dispatch module units use preset -O3, compile jobs="
    "${BIOSEQ_GCC_TYPED_DISPATCH_JOBS}, template force-inline="
    "${BIOSEQ_GCC_TEMPLATE_FORCE_INLINE}")
  unset(BIOSEQ_GCC_HEAVY_DISPATCH_IMPLEMENTATION_UNITS)
endif()

# The unit target alone compiles the event listener through its TESTS closure.
# Both core Catch executables link the shared indexing fakes; every Domain N
# module is delivered by bioseq_modules_nopg. The integration target adds its
# integration-specific modules below.
set(BIOSEQ_TEST_TARGET_ARGUMENTS
  TYPE EXEC
  CXX 26
  RELOCATABLE
  COMMENT "BioSeq tests"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include"
    "${CMAKE_CURRENT_LIST_DIR}/backend"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/safestringlib/include"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include"
  TESTS
    ${BIOSEQ_UNIT_TEST_SUPPORT_SOURCES}
    ${BIOSEQ_UNIT_TEST_SOURCES}
  INTEGRATION_TESTS
    ${BIOSEQ_INTEGRATION_TEST_SOURCES}
  INTEGRATION_TESTS_LINKS
    sqlgen::sqlgen
    bioseq_integration_test_support
    kseq++::kseq++
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
    bioseq_pg_support
    bioseq_bench_prefilter_metrics
    bioseq_testing_support
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)

bioseq_add_postgresql_extension_variant(
  bioseq_unit_tests
  ON
  ${BIOSEQ_TEST_TARGET_ARGUMENTS})
set_target_properties(bioseq_unit_tests PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)

bioseq_add_postgresql_extension_variant(
  bioseq_integration_tests
  INTEGRATION
  ${BIOSEQ_TEST_TARGET_ARGUMENTS})
set_target_properties(bioseq_integration_tests PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_integration_tests PRIVATE
  "BIOSEQ_IO_TEST_DATA_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../data\"")
unset(BIOSEQ_TEST_TARGET_ARGUMENTS)

if(BUILD_TESTING)
  add_test(
    NAME bioseq_unit_tests
    COMMAND "$<TARGET_FILE:bioseq_unit_tests>" "[component:bioseq]")
  set_tests_properties(bioseq_unit_tests PROPERTIES
    LABELS
      "component:bioseq;layer:unit;feature:alignment;feature:alphabet;feature:chaining;feature:configuration;feature:gpu_runtime;feature:index;feature:io;feature:minimizer;feature:pipeline;feature:planner;feature:prefilter;feature:scheduler;feature:seeding;feature:sequence;feature:simd_primitives;feature:sql_api;feature:storage;resource:filesystem;resource:gpu;resource:highway;resource:large_memory;resource:subprocess")
  add_test(
    NAME bioseq_integration_tests
    COMMAND "$<TARGET_FILE:bioseq_integration_tests>"
      "[component:bioseq][layer:integration]")
  set_tests_properties(bioseq_integration_tests PROPERTIES
    LABELS
      "component:bioseq;layer:integration;feature:alignment;feature:alphabet;feature:configuration;feature:extension_lifecycle;feature:index;feature:io;feature:pipeline;feature:planner;feature:prefilter;feature:scheduler;feature:sequence;feature:sql_api;feature:storage;resource:filesystem;resource:highway;resource:large_memory;resource:postgres;resource:subprocess"
    RESOURCE_LOCK "bioseq_private_postgres")
endif()

# Unit tests use one generator-proven fixed target, chosen from the compiler's
# HWY_STATIC_TARGET record. The test provider imports the same generated
# canonical partitions as production and never performs runtime dispatch.
if(TARGET bioseq_unit_tests)
  if(BIOSEQ_TEST_INDEXES)
    set_source_files_properties(
      ${BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS}
      PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-x;c++")
    target_sources(bioseq_unit_tests PRIVATE
      ${BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS})
  endif()
  target_link_libraries(bioseq_unit_tests PRIVATE
    bioseq_highway_test_support)
  add_dependencies(
    bioseq_unit_tests bioseq_highway_target_modules)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_sql_codegen.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/postgres_error_safety.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/postgres_error_safety_runtime.cmake")

target_link_libraries(bioseq_modules_nopg
  PUBLIC bioseq_sql_annotation_model)

add_library(bioseq_sql_annotation_probe OBJECT
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cpp")
target_sources(bioseq_sql_annotation_probe
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../codegen"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cppm")
target_link_libraries(bioseq_sql_annotation_probe
  PRIVATE bioseq_sql_annotation_model)
target_compile_options(bioseq_sql_annotation_probe
  PRIVATE
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
    "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
set_target_properties(bioseq_sql_annotation_probe PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON)

# The collector receives the configured backend module closure and ignores
# modules without bioseq_sql annotations.
option(BIOSEQ_TEST_INDEXES
  "Compile the test-only indexing descriptors into the extension + test execs"
  OFF)
set(BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/row_codec_runtime_test_support.cppm")
set(BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES
  "${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE}"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/contract_core.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/catalog_lifecycle.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/toast_faults.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/row_codec_runtime_test_support_parts/codec_fixtures.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES})
# The test-indexes extension imports and owns this provider. Production keeps
# it out of the DSO; a dedicated excluded target declared after the generated
# SQL provider closure supplies its raw compile command to dual-profile PREPARE.
if(BIOSEQ_TEST_INDEXES)
  list(APPEND SRC_MOD_FILES
    ${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES})
endif()
set(BIOSEQ_SQL_CODEGEN_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql")
set(BIOSEQ_SQL_PROFILE_ARTIFACT_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/profiles")
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_profile_artifact_dir
    "${BIOSEQ_SQL_PROFILE_ARTIFACT_ROOT}/${_bioseq_sql_profile}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL
    "${_bioseq_sql_profile_artifact_dir}/bioseq--1.0.sql")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST
    "${_bioseq_sql_profile_artifact_dir}/bioseq-sql-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP
    "${_bioseq_sql_profile_artifact_dir}/bioseq-sql.stamp")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST
    "${_bioseq_sql_profile_artifact_dir}/support-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP
    "${_bioseq_sql_profile_artifact_dir}/support.stamp")
endforeach()
if(BIOSEQ_TEST_INDEXES)
  set(_bioseq_expected_typed_sql_support_stamp
    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}")
else()
  set(_bioseq_expected_typed_sql_support_stamp
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}")
endif()
if(NOT "${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}" STREQUAL
    "${_bioseq_expected_typed_sql_support_stamp}")
  message(FATAL_ERROR
    "Template-flatten SQL support dependency does not match the active profile")
endif()
unset(_bioseq_expected_typed_sql_support_stamp)
set(BIOSEQ_SQL_CORE_DESCRIPTOR_HASH
  "${BIOSEQ_SQL_CODEGEN_DIR}/core/core-descriptor.sha256")
set(BIOSEQ_SQL_CORE_MANIFEST
  "${BIOSEQ_SQL_CODEGEN_DIR}/core/core-manifest.json")
set(BIOSEQ_SQL_CORE_GENERATED_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/core/core.stamp")
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_GENERATED_INSTALL
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_INSTALL}")
  set(BIOSEQ_SQL_GENERATED_MANIFEST
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}")
  set(BIOSEQ_SQL_GENERATED_STAMP
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_STAMP}")
else()
  set(BIOSEQ_SQL_GENERATED_INSTALL
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}")
  set(BIOSEQ_SQL_GENERATED_MANIFEST
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}")
  set(BIOSEQ_SQL_GENERATED_STAMP
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_STAMP}")
endif()
set(BIOSEQ_SQL_VALIDATOR
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/validate_postgres_error_safety.py")
set(BIOSEQ_SQL_SAFETY_PROVENANCE
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/tests/contract/fixtures/pg17_provenance.json")
set(BIOSEQ_SQL_SAFETY_RUNTIME_POLICY
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/policy/postgres_error_safety_policy.json")
set(BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE "" CACHE FILEPATH
  "Reviewed profile-neutral PostgreSQL runtime case template")
set(BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR "" CACHE PATH
  "Inverse-profile runtime-instrumented Bioseq build tree")
set(BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE "isolated" CACHE STRING
  "Runtime validation cluster relationship (isolated only)")
set_property(CACHE BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE
  PROPERTY STRINGS isolated)
set(BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS "600" CACHE STRING
  "Per-child timeout for PostgreSQL runtime validation")
mark_as_advanced(
  BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE
  BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR
  BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE
  BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS)
set(BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/validator-self-test.stamp")
set(BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/structural_analysis/tests/integration/test_prepare_incremental.py")
set(BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/prepare-incremental-self-test.stamp")
if(NOT EXISTS "${BIOSEQ_SQL_VALIDATOR}")
  message(FATAL_ERROR
    "Missing PostgreSQL safety validator: ${BIOSEQ_SQL_VALIDATOR}")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST)
  if(NOT EXISTS "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}")
    message(FATAL_ERROR
      "Missing PREPARE incremental fixture: "
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}")
  endif()
  find_program(BIOSEQ_SQL_PREPARE_NINJA
    NAMES ninja ninja-build
    REQUIRED)
  mark_as_advanced(BIOSEQ_SQL_PREPARE_NINJA)
endif()
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CODEGEN_DIR}"
  COMMAND "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_VALIDATOR}" --self-test
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
  DEPENDS
    "${BIOSEQ_SQL_VALIDATOR}"
    "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
  COMMENT "Running PostgreSQL safety validator self-tests"
  VERBATIM)
add_custom_target(bioseq_sql_validator_self_test
  DEPENDS "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}")
set(BIOSEQ_SQL_NEGATIVE_FIXTURES
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_nested_annotation_namespace.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_class_head_placement.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_guc_enumerator_placement.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/invalid_mixed_guc_alias_initialization.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_catalog_handle_specialization.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_guc_registration_access.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_catalog_handle_namespace_alias.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_guc_access_using_declaration.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/negative/authored_guc_registration_type_alias.cppm")
set(BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/negative-contracts.stamp")
if(BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST)
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test sql
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      ${BIOSEQ_SQL_NEGATIVE_FIXTURES}
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/index.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/options.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/index.cppm"
    COMMENT
      "Running real structural SQL negative/provenance contract fixtures"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}"
    COMMENT "Skipping disabled maintainer SQL negative/provenance fixtures"
    VERBATIM)
endif()
add_custom_target(bioseq_sql_negative_contracts
  DEPENDS "${BIOSEQ_SQL_NEGATIVE_CONTRACT_STAMP}")
if(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST)
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "${Python3_EXECUTABLE}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}" run
      --codegen "$<TARGET_FILE:bioseq_codegen>"
      --ninja "${BIOSEQ_SQL_PREPARE_NINJA}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256
        "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    COMMENT "Running bounded Ninja PREPARE incremental contract"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${BIOSEQ_SQL_CODEGEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    COMMENT "Skipping disabled maintainer PREPARE incremental contract"
    VERBATIM)
endif()
add_custom_target(bioseq_sql_prepare_incremental_self_test
  DEPENDS "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}")
set(BIOSEQ_SQL_ENTITY_HANDLES_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq/sql/core/owners")
set(BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE
  "${BIOSEQ_SQL_ENTITY_HANDLES_DIR}/storage_metadata_core.entity_handles.cppm")
set(BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED
  "${BIOSEQ_SQL_ENTITY_HANDLES_DIR}/storage_indexing_pg_chunked_storage.entity_handles.cppm")
set(BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE
  "${BIOSEQ_SQL_ENTITY_HANDLES_DIR}/storage_manager_sequence_storage_manager.entity_handles.cppm")
set(BIOSEQ_SQL_ENTITY_HANDLE_MODULES
  "${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
  "${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
  "${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}")
set(BIOSEQ_SQL_PROFILE_MODULE_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq/sql")
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_row_codecs_dir
    "${BIOSEQ_SQL_PROFILE_MODULE_ROOT}/${_bioseq_sql_profile}/owners")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA
    "${_bioseq_sql_row_codecs_dir}/storage_metadata.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG
    "${_bioseq_sql_row_codecs_dir}/storage_indexing_pg.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG
    "${_bioseq_sql_row_codecs_dir}/storage_load_pg.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER
    "${_bioseq_sql_row_codecs_dir}/storage_manager.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE
    "${_bioseq_sql_row_codecs_dir}/table.row_codecs.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODEC_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE}")

  set(_bioseq_sql_fmgr_entrypoints_dir
    "${BIOSEQ_SQL_PROFILE_MODULE_ROOT}/${_bioseq_sql_profile}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD
    "${_bioseq_sql_fmgr_entrypoints_dir}/mod.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT
    "${_bioseq_sql_fmgr_entrypoints_dir}/highway_target_test_support.entrypoints.cppm")
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    set(BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT
      "${_bioseq_sql_fmgr_entrypoints_dir}/highway_ksw_target_test_support.entrypoints.cppm")
  endif()
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER
    "${_bioseq_sql_fmgr_entrypoints_dir}/storage_manager_sequence_storage_manager.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS
    "${_bioseq_sql_fmgr_entrypoints_dir}/table_access_hooks.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG
    "${_bioseq_sql_fmgr_entrypoints_dir}/storage_load_pg.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF
    "${_bioseq_sql_fmgr_entrypoints_dir}/sequences_sample_table_udf.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE
    "${_bioseq_sql_fmgr_entrypoints_dir}/postgres_types_compact_sequence.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_types_integer.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_types_date.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_types_aggregates.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_comparison_registry.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY
    "${_bioseq_sql_fmgr_entrypoints_dir}/vector_aggregate_registry.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINT_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}")
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    list(APPEND BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES
      "${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT}")
  endif()
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_FMGR_ENTRYPOINT_MODULES
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}")
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    list(APPEND BIOSEQ_SQL_TEST_INDEXES_FINAL_FMGR_ENTRYPOINT_MODULES
      "${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT}")
  endif()
endforeach()
if(DEFINED BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT)
  message(FATAL_ERROR
    "KSW boundary FMGR output must not exist in the production SQL profile")
endif()
set(_bioseq_ksw_test_fmgr_occurrences 0)
foreach(_bioseq_fmgr_module
    IN LISTS BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES)
  if(_bioseq_fmgr_module STREQUAL
      BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT)
    math(EXPR _bioseq_ksw_test_fmgr_occurrences
      "${_bioseq_ksw_test_fmgr_occurrences} + 1")
  endif()
endforeach()
if(NOT _bioseq_ksw_test_fmgr_occurrences EQUAL 1
    OR BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT
      IN_LIST BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES)
  message(FATAL_ERROR
    "KSW boundary FMGR must occur exactly once in test_indexes and never "
    "in the production generated-module list")
endif()
unset(_bioseq_fmgr_module)
unset(_bioseq_ksw_test_fmgr_occurrences)
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_ROW_CODEC_MODULES
    ${BIOSEQ_SQL_TEST_INDEXES_ROW_CODEC_MODULES})
  set(BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES
    ${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES})
  set(_bioseq_sql_active_profile test_indexes)
  set(_bioseq_sql_active_profile_key TEST_INDEXES)
else()
  set(BIOSEQ_SQL_ROW_CODEC_MODULES
    ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES})
  set(BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES
    ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES})
  set(_bioseq_sql_active_profile production)
  set(_bioseq_sql_active_profile_key PRODUCTION)
endif()
foreach(_bioseq_sql_output_name IN ITEMS
    ROW_CODECS_STORAGE_METADATA
    ROW_CODECS_STORAGE_INDEXING_PG
    ROW_CODECS_STORAGE_LOAD_PG
    ROW_CODECS_STORAGE_MANAGER
    ROW_CODECS_TABLE
    FMGR_ENTRYPOINTS_MOD
    FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT
    FMGR_ENTRYPOINTS_STORAGE_MANAGER
    FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS
    FMGR_ENTRYPOINTS_STORAGE_LOAD_PG
    FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF
    FMGR_ENTRYPOINTS_COMPACT_SEQUENCE
    FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER
    FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE
    FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES
    VECTOR_COMPARISON_REGISTRY
    VECTOR_AGGREGATE_REGISTRY)
  set(BIOSEQ_SQL_${_bioseq_sql_output_name}
    "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_${_bioseq_sql_output_name}}")
endforeach()
set(_bioseq_sql_completion_args
  --require-complete-codegen)
set(_bioseq_sql_completion_target_option
  REQUIRE_COMPLETE_CODEGEN)
set(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR "" CACHE PATH
  "Reviewed production/test-index PostgreSQL safety validation bundle")
mark_as_advanced(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR)
set(_bioseq_sql_safety_review_inputs)
if(NOT "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}" STREQUAL "")
  get_filename_component(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}" ABSOLUTE)
  set(BIOSEQ_SQL_SAFETY_PRODUCTION_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/production-ledger.json")
  set(BIOSEQ_SQL_SAFETY_TEST_INDEXES_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/test-indexes-ledger.json")
  set(BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/reviewed/production-ledger.json")
  set(BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/reviewed/test-indexes-ledger.json")
  set(BIOSEQ_SQL_SAFETY_RUNTIME_MATRIX
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/runtime-matrix.json")
  set(BIOSEQ_SQL_SAFETY_REVIEWED_RUNTIME_MATRIX
    "${BIOSEQ_SQL_SAFETY_EVIDENCE_DIR}/reviewed/runtime-matrix.json")
  set(_bioseq_sql_safety_review_inputs
    "${BIOSEQ_SQL_SAFETY_PRODUCTION_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_MATRIX}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_RUNTIME_MATRIX}")
  foreach(_bioseq_sql_safety_input
      IN LISTS _bioseq_sql_safety_review_inputs)
    if(NOT EXISTS "${_bioseq_sql_safety_input}")
      message(FATAL_ERROR
        "Incomplete PostgreSQL safety validation bundle: missing "
        "${_bioseq_sql_safety_input}")
    endif()
  endforeach()
endif()
set(BIOSEQ_SQL_GUC_OUTPUT_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq/sql/gucs")
set(BIOSEQ_SQL_GUC_GENERATED_MODULES)
foreach(_bioseq_guc_profile IN ITEMS production test_indexes)
  set(_bioseq_guc_profile_modules)
  foreach(_bioseq_guc_owner IN ITEMS bioseq scheduler storage aligner)
    list(APPEND _bioseq_guc_profile_modules
      "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/${_bioseq_guc_profile}/owners/${_bioseq_guc_owner}.gucs.cppm")
  endforeach()
  list(APPEND _bioseq_guc_profile_modules
    "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/${_bioseq_guc_profile}/guc_registry.cppm")
  set(BIOSEQ_SQL_GUC_${_bioseq_guc_profile}_MODULES
    "${_bioseq_guc_profile_modules}")
  list(APPEND BIOSEQ_SQL_GUC_GENERATED_MODULES
    ${_bioseq_guc_profile_modules})
endforeach()
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_GUC_ACTIVE_PROFILE test_indexes)
  set(BIOSEQ_SQL_GUC_ACTIVE_MODULES
    ${BIOSEQ_SQL_GUC_test_indexes_MODULES})
else()
  set(BIOSEQ_SQL_GUC_ACTIVE_PROFILE production)
  set(BIOSEQ_SQL_GUC_ACTIVE_MODULES
    ${BIOSEQ_SQL_GUC_production_MODULES})
endif()

# SQL ownership is an explicit configure-time contract.  These registries are
# the only inputs accepted by the collector; ordinary target source lists are
# intentionally not consulted.  SUPPORT sources are dependency-only
# declaration modules, CORE is profile-neutral authority, and FINAL is the full
# profile closure consumed by final SQL and boundary generation.
bioseq_sql_begin_owner_registry(
  ROOT "${CMAKE_CURRENT_LIST_DIR}"
  PROFILES production test_indexes
  AUDIT_ROOTS backend)

bioseq_register_sql_owner(
  OWNER_KEY aligner_guc
  MODULE bioseq.postgres.configuration.aligner
  SOURCES backend/pg/configuration/aligner.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS aligner)
bioseq_register_sql_owner(
  OWNER_KEY scheduler_guc
  MODULE bioseq.postgres.configuration.scheduler
  SOURCES backend/pg/configuration/scheduler.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS scheduler)
bioseq_register_sql_owner(
  OWNER_KEY extension_guc
  MODULE bioseq.postgres.configuration.core
  SOURCES backend/pg/configuration/core.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS bioseq)
bioseq_register_sql_owner(
  OWNER_KEY storage_guc
  MODULE bioseq.postgres.configuration.storage
  SOURCES backend/pg/configuration/storage.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS storage)

bioseq_register_sql_owner(
  OWNER_KEY alignment_rows
  MODULE bioseq.postgres.extension.alignment.sql_rows
  SOURCES backend/pg/extension/alignment_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY storage_metadata_rows
  MODULE bioseq.postgres.storage.metadata.sql_rows
  SOURCES backend/pg/column/metadata/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_metadata)
bioseq_register_sql_owner(
  OWNER_KEY mod_rows
  MODULE bioseq.postgres.extension.sql_rows
  SOURCES backend/pg/extension/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY sequences_sample_table_udf_rows
  MODULE bioseq.postgres.sequences.sample_table.sql_rows
  SOURCES backend/pg/sequences/sample_table/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ANNOTATION_FREE_RESULT_ROWS sequences_sample_table_udf)
bioseq_register_sql_owner(
  OWNER_KEY sql_function_rows
  MODULE bioseq.postgres.sql.functions.sql_rows
  SOURCES backend/pg/sql/functions_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY storage_indexing_rows
  MODULE bioseq.postgres.storage.indexing.sql_rows
  SOURCES backend/pg/storage/indexing/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_indexing_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_load_rows
  MODULE bioseq.postgres.storage.io.sql_rows
  SOURCES backend/pg/storage/io/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_load_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_rows
  MODULE bioseq.postgres.storage.manager.sql_rows
  SOURCES backend/pg/storage/manager/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY table_rows
  MODULE bioseq.postgres.table.sql_rows
  SOURCES backend/pg/table/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS table)

bioseq_register_sql_owner(
  OWNER_KEY storage_metadata_core
  MODULE bioseq.postgres.storage.metadata.sql_entities
  SOURCES backend/pg/column/metadata/sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_metadata_core)
bioseq_register_sql_owner(
  OWNER_KEY mod
  MODULE bioseq.postgres.extension
  SOURCES backend/pg/extension/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS mod)
bioseq_register_sql_owner(
  OWNER_KEY highway_target_test_support
  MODULE bioseq.highway_target_test_support
  SOURCES backend/pg/highway_target_test_support.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS highway_target_test_support)
bioseq_register_sql_owner(
  OWNER_KEY highway_ksw_target_test_support
  MODULE bioseq.highway_ksw_target_test_support
  SOURCES backend/pg/highway_ksw_target_test_support.cppm
  PROFILES test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS highway_ksw_target_test_support)
bioseq_register_sql_owner(
  OWNER_KEY sequences_sample_table_udf
  MODULE bioseq.postgres.sequences.sample_table
  SOURCES backend/pg/sequences/sample_table/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS sequences_sample_table_udf)
bioseq_register_sql_owner(
  OWNER_KEY sql_bootstrap
  MODULE bioseq.postgres.sql.bootstrap
  SOURCES backend/pg/sql/bootstrap.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY sql_catalog_views
  MODULE bioseq.postgres.sql.catalog_views
  SOURCES backend/pg/sql/catalog_views.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY sql_functions
  MODULE bioseq.postgres.sql.functions
  SOURCES backend/pg/sql/functions.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY external_relation_fmgr_export
  MODULE bioseq.postgres.external.relation
  SOURCES backend/pg/external/relation.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  HANDWRITTEN_FMGR_EXPORTS bioseq_external_handler)
bioseq_register_sql_owner(
  OWNER_KEY external_sql_catalog
  MODULE bioseq.postgres.external.sql_catalog
  SOURCES backend/pg/external/sql_catalog.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY storage_indexing_pg_chunked_storage
  MODULE bioseq.postgres.storage.indexing.chunked_storage.sql_entities
  SOURCES backend/pg/storage/indexing/chunked_storage/sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_indexing_pg_chunked_storage)
bioseq_register_sql_owner(
  OWNER_KEY storage_load_pg
  MODULE bioseq.postgres.storage.io
  SOURCES backend/pg/storage/io/index.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS storage_load_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_sequence_storage_manager_core
  MODULE bioseq.postgres.storage.manager.sequence_storage.sql_entities
  SOURCES backend/pg/storage/manager/sequence_storage_sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_manager_sequence_storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_sequence_storage_manager
  MODULE bioseq.postgres.storage.manager:sequence_storage
  SOURCES backend/pg/storage/manager/sequence_storage.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS storage_manager_sequence_storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY table
  MODULE bioseq.postgres.table
  SOURCES
    backend/pg/table/access_hooks.cppm
    backend/pg/table/access_method.cppm
    backend/pg/table/custom_scan.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS table_access_hooks)
bioseq_register_sql_owner(
  OWNER_KEY postgres_types_compact_sequence
  MODULE bioseq.postgres.types.compact_sequence
  SOURCES
    backend/pg/types/compact_sequence/array_string.cppm
    backend/pg/types/compact_sequence/concat_reverse.cppm
    backend/pg/types/compact_sequence/content.cppm
    backend/pg/types/compact_sequence/flat_abi.cppm
    backend/pg/types/compact_sequence/io.cppm
    backend/pg/types/compact_sequence/letter.cppm
    backend/pg/types/compact_sequence/packed_output.cppm
    backend/pg/types/compact_sequence/packed_output_parts/materialization_trace.cppm
    backend/pg/types/compact_sequence/packed_output_parts/async_ownership.cppm
    backend/pg/types/compact_sequence/packed_output_parts/packing.cppm
    backend/pg/types/compact_sequence/packed_output_parts/test_contracts.cppm
    backend/pg/types/compact_sequence/sql_catalog.cppm
    backend/pg/types/compact_sequence/subscript.cppm
    backend/pg/types/compact_sequence/tree_ops.cppm
    backend/pg/types/compact_sequence/tree_ops_parts/ownership.cppm
    backend/pg/types/compact_sequence/tree_ops_parts/transforms.cppm
    backend/pg/types/compact_sequence/tree_ops_parts/validation.cppm
    backend/pg/types/compact_sequence/tree_ops_parts/failure_contracts.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS postgres_types_compact_sequence)
bioseq_register_sql_owner(
  OWNER_KEY vector_types
  MODULE bioseq.postgres.vectorization.types
  SOURCES
    backend/pg/vectorization/types/aggregates.cppm
    backend/pg/vectorization/types/date.cppm
    backend/pg/vectorization/types/int.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS
    vector_types_integer
    vector_types_date
    vector_types_aggregates
    vector_comparison_registry
    vector_aggregate_registry
  FMGR_OWNER_MODULES
    vector_types_integer=bioseq.postgres.vectorization.types.integer
    vector_types_date=bioseq.postgres.vectorization.types.date
    vector_types_aggregates=bioseq.postgres.vectorization.types.aggregates)

set(BIOSEQ_SQL_REGISTRY_DIR
  "${BIOSEQ_SQL_CODEGEN_DIR}/registries")
bioseq_finalize_sql_owner_registry(
  OUTPUT_DIR "${BIOSEQ_SQL_REGISTRY_DIR}"
  EXPECTED_ROW_CODECS
    storage_metadata
    storage_indexing_pg
    storage_load_pg
    storage_manager
    table
  EXPECTED_ENTITY_HANDLES
    storage_metadata_core
    storage_indexing_pg_chunked_storage
    storage_manager_sequence_storage_manager
  EXPECTED_FMGR_ENTRYPOINTS
    mod
    highway_target_test_support
    highway_ksw_target_test_support
    storage_manager_sequence_storage_manager
    table_access_hooks
    storage_load_pg
    sequences_sample_table_udf
    postgres_types_compact_sequence
    vector_types_integer
    vector_types_date
    vector_types_aggregates
    vector_comparison_registry
    vector_aggregate_registry
  EXPECTED_GUC_OWNERS
    bioseq
    scheduler
    storage
    aligner
  EXPECTED_ANNOTATION_FREE_RESULT_ROWS
    sequences_sample_table_udf
  EXPECTED_HANDWRITTEN_FMGR_EXPORTS
    bioseq_external_handler)

get_property(_bioseq_sql_entity_handle_owner_records GLOBAL
  PROPERTY BIOSEQ_SQL_BIOSEQ_ENTITY_HANDLE_OWNER_RECORDS)
list(SORT _bioseq_sql_entity_handle_owner_records)
set(_bioseq_sql_entity_handle_owner_args)
foreach(_bioseq_sql_entity_handle_owner_record IN LISTS
    _bioseq_sql_entity_handle_owner_records)
  list(APPEND _bioseq_sql_entity_handle_owner_args
    --entity-handle-owner-module
    "${_bioseq_sql_entity_handle_owner_record}")
endforeach()

# Clang collection never consumes a GCC BMI or the main target's compiler
# cache.  Each authority/profile prepare command derives a normalized,
# profile-checked Clang database from CMake's raw database and populates only
# its content-keyed PCM root.  SUPPORT and CORE can therefore bootstrap a clean
# tree before FINAL owners import their generated modules.
set(BIOSEQ_SQL_RAW_COMPILE_DATABASE
  "${CMAKE_BINARY_DIR}/compile_commands.json"
  CACHE FILEPATH
  "Raw CMake compile database used to prepare owned SQL Clang profiles")
mark_as_advanced(BIOSEQ_SQL_RAW_COMPILE_DATABASE)
set(BIOSEQ_SQL_CLANG_CONTEXT_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/clang")
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}")

foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_support_registry
    "${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_SUPPORT_FILE}")
  set(_bioseq_sql_support_sources
    ${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_SUPPORT_SOURCES})
  set(_bioseq_sql_pcm_root
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/pcm")
  set(_bioseq_sql_support_compile_database
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/compile_commands.json")
  set(_bioseq_sql_support_prepare_stamp
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/prepare.stamp")
  set(_bioseq_sql_support_prepare_depfile
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/prepare.d")
  set(_bioseq_sql_support_pcm_manifest
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/support/pcm-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_PCM_ROOT
    "${_bioseq_sql_pcm_root}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_COMPILE_DATABASE
    "${_bioseq_sql_support_compile_database}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_PREPARE_STAMP
    "${_bioseq_sql_support_prepare_stamp}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_PCM_MANIFEST
    "${_bioseq_sql_support_pcm_manifest}")

  add_custom_command(
    OUTPUT "${_bioseq_sql_support_prepare_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_support_compile_database}"
      "${_bioseq_sql_support_pcm_manifest}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${_bioseq_sql_pcm_root}"
    COMMAND
      "$<TARGET_FILE:bioseq_codegen>" sql prepare
      --authority support
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_support_registry}"
      --raw-compile-database "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --compile-database-output
        "${_bioseq_sql_support_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_support_pcm_manifest}"
      --depfile "${_bioseq_sql_support_prepare_depfile}"
      --stamp "${_bioseq_sql_support_prepare_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
      "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      "${_bioseq_sql_support_registry}"
      "${BIOSEQ_SQL_OWNER_REGISTRY_METADATA}"
      ${_bioseq_sql_support_sources}
    COMMENT
      "Preparing ${_bioseq_sql_profile} Clang SQL support closure"
    DEPFILE "${_bioseq_sql_support_prepare_depfile}"
    VERBATIM)
  add_custom_target(
    bioseq_sql_prepare_support_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_support_prepare_stamp}")

  set(_bioseq_sql_row_codec_modules
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODEC_MODULES})
  get_property(_bioseq_sql_row_codec_owner_records GLOBAL
    PROPERTY
      "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_ROW_CODEC_OWNER_RECORDS")
  list(SORT _bioseq_sql_row_codec_owner_records)
  set(_bioseq_sql_row_codec_owner_args)
  foreach(_bioseq_sql_row_codec_owner_record IN LISTS
      _bioseq_sql_row_codec_owner_records)
    list(APPEND _bioseq_sql_row_codec_owner_args
      --row-codec-owner-module
      "${_bioseq_sql_row_codec_owner_record}")
  endforeach()
  set(_bioseq_sql_guc_modules
    ${BIOSEQ_SQL_GUC_${_bioseq_sql_profile}_MODULES})
  set(_bioseq_sql_support_vector_registry_modules
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES})
  set(_bioseq_sql_support_manifest
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST}")
  set(_bioseq_sql_support_stamp
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}")
  add_custom_command(
    OUTPUT "${_bioseq_sql_support_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_support_manifest}"
      ${_bioseq_sql_row_codec_modules}
      ${_bioseq_sql_guc_modules}
      ${_bioseq_sql_support_vector_registry_modules}
    COMMAND "${CMAKE_COMMAND}" -E env
      "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
      "$<TARGET_FILE:bioseq_codegen>" sql generate
      --stage support
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_support_registry}"
      --compile-database "${_bioseq_sql_support_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_support_pcm_manifest}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --manifest "${_bioseq_sql_support_manifest}"
      --stamp "${_bioseq_sql_support_stamp}"
      --row-codecs-output
        "storage_metadata=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA}"
      --row-codecs-output
        "storage_indexing_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
      --row-codecs-output
        "storage_load_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
      --row-codecs-output
        "storage_manager=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
      --row-codecs-output
        "table=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE}"
      ${_bioseq_sql_row_codec_owner_args}
      --fmgr-entrypoints-output
        "vector_comparison_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
      --fmgr-entrypoints-output
        "vector_aggregate_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}"
      --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
      --guc-active-profile "${_bioseq_sql_profile}"
      --generated-module-prefix bioseq.postgres.sql.generated
      --extension-schema public
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${_bioseq_sql_support_prepare_stamp}"
      "${_bioseq_sql_support_compile_database}"
      "${_bioseq_sql_support_pcm_manifest}"
      "${_bioseq_sql_support_registry}"
      ${_bioseq_sql_support_sources}
    COMMENT
      "Generating ${_bioseq_sql_profile} SQL support modules"
    VERBATIM)
  add_custom_target(
    bioseq_sql_support_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_support_stamp}")
  add_dependencies(
    bioseq_sql_support_${_bioseq_sql_profile}
    bioseq_sql_prepare_support_${_bioseq_sql_profile})
endforeach()

set(BIOSEQ_SQL_CORE_PCM_ROOT
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/pcm")
set(BIOSEQ_SQL_CORE_COMPILE_DATABASE
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/compile_commands.json")
set(BIOSEQ_SQL_CORE_PREPARE_STAMP
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/prepare.stamp")
set(BIOSEQ_SQL_CORE_PREPARE_DEPFILE
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/prepare.d")
set(BIOSEQ_SQL_CORE_PCM_MANIFEST
  "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/core/pcm-manifest.json")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_CORE_PREPARE_STAMP}"
  BYPRODUCTS
    "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CORE_PCM_ROOT}"
  COMMAND
    "$<TARGET_FILE:bioseq_codegen>" sql prepare
    --authority core
    --profile production
    --source-list "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    --raw-compile-database "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    --compiler-module-inventory
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
    --clang-scan-deps-sha256
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
    --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
    --compiler-module-inventory-sha256
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    --compile-database-output "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    --pcm-root "${BIOSEQ_SQL_CORE_PCM_ROOT}"
    --pcm-manifest "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
    --depfile "${BIOSEQ_SQL_CORE_PREPARE_DEPFILE}"
    --stamp "${BIOSEQ_SQL_CORE_PREPARE_STAMP}"
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
    "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
    "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    "${BIOSEQ_SQL_OWNER_REGISTRY_METADATA}"
    ${BIOSEQ_SQL_REGISTRY_CORE_SOURCES}
  COMMENT "Preparing profile-neutral Clang SQL core closure"
  DEPFILE "${BIOSEQ_SQL_CORE_PREPARE_DEPFILE}"
  VERBATIM)
add_custom_target(bioseq_sql_prepare_core
  DEPENDS "${BIOSEQ_SQL_CORE_PREPARE_STAMP}")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
  BYPRODUCTS
    "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    "${BIOSEQ_SQL_CORE_MANIFEST}"
    ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
  COMMAND "${CMAKE_COMMAND}" -E env
    "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
    "$<TARGET_FILE:bioseq_codegen>" sql generate
    --stage core
    --profile production
    --source-list "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    --compile-database "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    --pcm-root "${BIOSEQ_SQL_CORE_PCM_ROOT}"
    --pcm-manifest "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
    --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    --compiler-module-inventory
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
    --clang-scan-deps-sha256
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
    --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
    --compiler-module-inventory-sha256
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    --core-hash-output "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    --core-manifest "${BIOSEQ_SQL_CORE_MANIFEST}"
    --stamp "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    --entity-handles-output
      "storage_metadata_core=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
    --entity-handles-output
      "storage_indexing_pg_chunked_storage=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
    --entity-handles-output
      "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}"
    ${_bioseq_sql_entity_handle_owner_args}
    --generated-module-prefix bioseq.postgres.sql.generated
    --extension-schema public
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
    "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    "${BIOSEQ_SQL_CORE_PREPARE_STAMP}"
    "${BIOSEQ_SQL_CORE_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_CORE_PCM_MANIFEST}"
    "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    ${BIOSEQ_SQL_REGISTRY_CORE_SOURCES}
  COMMENT "Generating profile-neutral SQL entity handles"
  VERBATIM)
add_custom_target(bioseq_sql_core_entity_handles
  DEPENDS "${BIOSEQ_SQL_CORE_GENERATED_STAMP}")
add_dependencies(
  bioseq_sql_core_entity_handles
  bioseq_sql_prepare_core)

set(_bioseq_sql_final_stamps)
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_final_registry
    "${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_FINAL_FILE}")
  set(_bioseq_sql_final_sources
    ${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_FINAL_SOURCES})
  set(_bioseq_sql_pcm_root
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_PCM_ROOT}")
  set(_bioseq_sql_final_compile_database
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/compile_commands.json")
  set(_bioseq_sql_final_prepare_stamp
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/prepare.stamp")
  set(_bioseq_sql_final_prepare_depfile
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/prepare.d")
  set(_bioseq_sql_final_pcm_manifest
    "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}/${_bioseq_sql_profile}/final/pcm-manifest.json")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_COMPILE_DATABASE
    "${_bioseq_sql_final_compile_database}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_PREPARE_STAMP
    "${_bioseq_sql_final_prepare_stamp}")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_PCM_MANIFEST
    "${_bioseq_sql_final_pcm_manifest}")

  add_custom_command(
    OUTPUT "${_bioseq_sql_final_prepare_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_final_compile_database}"
      "${_bioseq_sql_final_pcm_manifest}"
    COMMAND
      "$<TARGET_FILE:bioseq_codegen>" sql prepare
      --authority final
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_final_registry}"
      --raw-compile-database "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --compile-database-output
        "${_bioseq_sql_final_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_final_pcm_manifest}"
      --depfile "${_bioseq_sql_final_prepare_depfile}"
      --stamp "${_bioseq_sql_final_prepare_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP}"
      "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
      "${_bioseq_sql_final_registry}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}"
      ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES}
      "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
      ${_bioseq_sql_final_sources}
    COMMENT
      "Preparing ${_bioseq_sql_profile} Clang SQL final closure"
    DEPFILE "${_bioseq_sql_final_prepare_depfile}"
    VERBATIM)
  add_custom_target(
    bioseq_sql_prepare_final_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_final_prepare_stamp}")
  add_dependencies(
    bioseq_sql_prepare_final_${_bioseq_sql_profile}
    bioseq_sql_support_${_bioseq_sql_profile}
    bioseq_sql_core_entity_handles)

  set(_bioseq_sql_generated_install
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}")
  set(_bioseq_sql_generated_manifest
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}")
  set(_bioseq_sql_generated_stamp
    "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}")
  set(_bioseq_sql_fmgr_modules
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FINAL_FMGR_ENTRYPOINT_MODULES})
  get_property(_bioseq_sql_row_codec_owner_records GLOBAL
    PROPERTY
      "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_ROW_CODEC_OWNER_RECORDS")
  list(SORT _bioseq_sql_row_codec_owner_records)
  set(_bioseq_sql_row_codec_owner_args)
  foreach(_bioseq_sql_row_codec_owner_record IN LISTS
      _bioseq_sql_row_codec_owner_records)
    list(APPEND _bioseq_sql_row_codec_owner_args
      --row-codec-owner-module
      "${_bioseq_sql_row_codec_owner_record}")
  endforeach()
  set(_bioseq_sql_profile_only_fmgr_args)
  if(_bioseq_sql_profile STREQUAL "test_indexes")
    list(APPEND _bioseq_sql_profile_only_fmgr_args
      --fmgr-entrypoints-output
      "highway_ksw_target_test_support=${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINTS_HIGHWAY_KSW_TARGET_TEST_SUPPORT}")
  endif()
  get_property(_bioseq_sql_fmgr_owner_records GLOBAL
    PROPERTY
      "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_FMGR_OWNER_RECORDS")
  list(SORT _bioseq_sql_fmgr_owner_records)
  set(_bioseq_sql_fmgr_owner_args)
  foreach(_bioseq_sql_fmgr_owner_record IN LISTS
      _bioseq_sql_fmgr_owner_records)
    list(APPEND _bioseq_sql_fmgr_owner_args
      --fmgr-owner-module "${_bioseq_sql_fmgr_owner_record}")
  endforeach()

  add_custom_command(
    OUTPUT "${_bioseq_sql_generated_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_generated_install}"
      "${_bioseq_sql_generated_manifest}"
      ${_bioseq_sql_fmgr_modules}
    COMMAND "${CMAKE_COMMAND}" -E env
      "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
      "$<TARGET_FILE:bioseq_codegen>" sql generate
      --stage final
      --profile "${_bioseq_sql_profile}"
      --source-list "${_bioseq_sql_final_registry}"
      --compile-database "${_bioseq_sql_final_compile_database}"
      --pcm-root "${_bioseq_sql_pcm_root}"
      --pcm-manifest "${_bioseq_sql_final_pcm_manifest}"
      --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      --compiler-module-inventory
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
      --clang-scan-deps-sha256
        "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
      --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
      --compiler-module-inventory-sha256
        "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
      --output "${_bioseq_sql_generated_install}"
      --manifest "${_bioseq_sql_generated_manifest}"
      --stamp "${_bioseq_sql_generated_stamp}"
      --support-manifest
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST}"
      --support-seal
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}"
      --production-support-manifest
        "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
      --production-support-seal
        "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
      --test-indexes-support-manifest
        "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
      --test-indexes-support-seal
        "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
      --expected-core-hash "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
      --core-manifest "${BIOSEQ_SQL_CORE_MANIFEST}"
      --entity-handles-output
        "storage_metadata_core=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
      --entity-handles-output
        "storage_indexing_pg_chunked_storage=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
      --entity-handles-output
        "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}"
      ${_bioseq_sql_entity_handle_owner_args}
      --row-codecs-output
        "storage_metadata=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_METADATA}"
      --row-codecs-output
        "storage_indexing_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
      --row-codecs-output
        "storage_load_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
      --row-codecs-output
        "storage_manager=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
      --row-codecs-output
        "table=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODECS_TABLE}"
      ${_bioseq_sql_row_codec_owner_args}
      --fmgr-entrypoints-output
        "mod=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_MOD}"
      --fmgr-entrypoints-output
        "highway_target_test_support=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
      --fmgr-entrypoints-output
        "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
      --fmgr-entrypoints-output
        "table_access_hooks=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
      --fmgr-entrypoints-output
        "storage_load_pg=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
      --fmgr-entrypoints-output
        "sequences_sample_table_udf=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
      --fmgr-entrypoints-output
        "postgres_types_compact_sequence=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
      --fmgr-entrypoints-output
        "vector_types_integer=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
      --fmgr-entrypoints-output
        "vector_types_date=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
      --fmgr-entrypoints-output
        "vector_types_aggregates=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}"
      --fmgr-entrypoints-output
        "vector_comparison_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
      --fmgr-entrypoints-output
        "vector_aggregate_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}"
      ${_bioseq_sql_profile_only_fmgr_args}
      ${_bioseq_sql_fmgr_owner_args}
      --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
      --guc-active-profile "${_bioseq_sql_profile}"
      ${_bioseq_sql_completion_args}
      --generated-module-prefix bioseq.postgres.sql.generated
      --extension-schema public
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${_bioseq_sql_final_prepare_stamp}"
      "${_bioseq_sql_final_compile_database}"
      "${_bioseq_sql_final_pcm_manifest}"
      "${_bioseq_sql_final_registry}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_STAMP}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_MANIFEST}"
      "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
      "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
      "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
      "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
      "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
      "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
      "${BIOSEQ_SQL_CORE_MANIFEST}"
      ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
      ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_ROW_CODEC_MODULES}
      ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_SUPPORT_VECTOR_REGISTRY_MODULES}
      ${BIOSEQ_SQL_GUC_production_MODULES}
      ${BIOSEQ_SQL_GUC_test_indexes_MODULES}
      "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
      ${_bioseq_sql_final_sources}
    COMMENT
      "Generating ${_bioseq_sql_profile} PostgreSQL extension SQL"
    VERBATIM)
  add_custom_target(
    bioseq_sql_final_${_bioseq_sql_profile}
    DEPENDS "${_bioseq_sql_generated_stamp}")
  add_dependencies(
    bioseq_sql_final_${_bioseq_sql_profile}
    bioseq_sql_prepare_final_${_bioseq_sql_profile})
  list(APPEND _bioseq_sql_final_stamps
    "${_bioseq_sql_generated_stamp}")
endforeach()

add_custom_target(bioseq_sql_final
  DEPENDS ${_bioseq_sql_final_stamps})
add_dependencies(
  bioseq_sql_final
  bioseq_sql_final_production
  bioseq_sql_final_test_indexes)

# The raw compile database names the generated core entity-handle providers and
# the active-profile row-codec providers before their source files exist on a
# fresh tree.  Template-flatten PREPARE reaches both while recursively scanning
# the scheduler primary.  Namespace materialization selects production compiler
# authority, so a test-indexes raw-database row is retargeted to its production
# sibling and both the active SUPPORT stage and production SUPPORT stage must be
# complete before the scan.  Do not wait for either FINAL profile: those much
# wider closures are not scheduler inputs.  Append the concrete stamps as file
# dependencies because target-only edges would allow Ninja to run the custom
# commands concurrently.
add_custom_command(
  OUTPUT
    "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}/pcm/metadata/prepare_compile_commands.json"
  APPEND
  DEPENDS
    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}")

set(BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY
  "${BIOSEQ_SQL_CODEGEN_DIR}/required-target-dyndeps.tsv")

# PREPARE consumes every C++ module row in CMake's raw compile database.
# Depending only on the extension dyndep therefore leaves unrelated, unique
# response-file classes (bench, cache, and compiler-contract fixtures)
# unmaterialized in a clean tree.  These top-level dyndep anchors are the
# bounded minimum that covers every current source+standard equivalence class
# through CMake's generated CXXModules dependencies.
#
# Resolve paths from target BINARY_DIR only after this directory has declared
# its benchmark targets. File dependencies materialize scans/modmaps without
# making compile targets depend on SQL validation; the extension link
# validation remains a link-only dependency below.
function(_bioseq_finalize_sql_target_dyndep_anchors)
  set(_bioseq_sql_target_dyndep_anchors
    bioseq_benchmark
    bioseq_benchmark_e2e_contract_tests
    bioseq_sql_annotation_probe
    bioseq_sql_gcc_compile_contract_objects
    postgresql_bioseq)
  if(TARGET bioseq_sql_row_codec_runtime_test_support_compile_donor)
    list(APPEND _bioseq_sql_target_dyndep_anchors
      bioseq_sql_row_codec_runtime_test_support_compile_donor)
  endif()
  if(BIOSEQ_BUILD_CACHE)
    list(APPEND _bioseq_sql_target_dyndep_anchors
      postgresql_bioseq_cache)
  endif()
  list(SORT _bioseq_sql_target_dyndep_anchors)

  string(CONCAT _bioseq_sql_target_dyndep_inventory
    "schema\tbioseq_sql_target_dyndep_inventory\n"
    "target\tdyndep\n")
  set(_bioseq_sql_target_dyndep_paths)
  foreach(_bioseq_sql_target IN LISTS
      _bioseq_sql_target_dyndep_anchors)
    if(NOT TARGET "${_bioseq_sql_target}")
      message(FATAL_ERROR
        "Required SQL PREPARE dyndep anchor target is absent: "
        "${_bioseq_sql_target}")
    endif()
    get_target_property(_bioseq_sql_target_binary_dir
      "${_bioseq_sql_target}" BINARY_DIR)
    if(NOT _bioseq_sql_target_binary_dir
       OR _bioseq_sql_target_binary_dir MATCHES "-NOTFOUND$")
      message(FATAL_ERROR
        "Required SQL PREPARE dyndep anchor has no BINARY_DIR: "
        "${_bioseq_sql_target}")
    endif()
    get_filename_component(_bioseq_sql_target_binary_dir
      "${_bioseq_sql_target_binary_dir}" ABSOLUTE)
    set(_bioseq_sql_target_dyndep
      "${_bioseq_sql_target_binary_dir}/CMakeFiles/${_bioseq_sql_target}.dir/CXX.dd")
    list(APPEND _bioseq_sql_target_dyndep_paths
      "${_bioseq_sql_target_dyndep}")
    string(APPEND _bioseq_sql_target_dyndep_inventory
      "${_bioseq_sql_target}\t${_bioseq_sql_target_dyndep}\n")
  endforeach()

  file(MAKE_DIRECTORY "${BIOSEQ_SQL_CODEGEN_DIR}")
  file(CONFIGURE
    OUTPUT "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"
    CONTENT "${_bioseq_sql_target_dyndep_inventory}"
    @ONLY
    NEWLINE_STYLE UNIX)
  foreach(_bioseq_sql_prepare_output IN ITEMS
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PREPARE_STAMP}")
    if("${_bioseq_sql_prepare_output}" STREQUAL "")
      message(FATAL_ERROR
        "SQL PREPARE output is unavailable during dyndep finalization")
    endif()
    add_custom_command(
      OUTPUT "${_bioseq_sql_prepare_output}"
      APPEND
      DEPENDS
        "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"
        ${_bioseq_sql_target_dyndep_paths})
  endforeach()
endfunction()
cmake_language(DEFER CALL
  _bioseq_finalize_sql_target_dyndep_anchors)

# Reusable provider closure for generated SQL C++ facets. The extension links
# these objects and PUBLIC BMIs, while the test-only OBJECT target below only
# consumes their BMIs to compile independent importers.
set(BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_ROW_CODECS
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_METADATA}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_INDEXING_PG}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_LOAD_PG}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_STORAGE_MANAGER}"
  "${BIOSEQ_SQL_${_bioseq_sql_active_profile_key}_ROW_CODECS_TABLE}")
set(BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_PROVIDERS
  ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
  ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_ROW_CODECS}
  ${BIOSEQ_SQL_GUC_ACTIVE_MODULES})
set(BIOSEQ_SQL_POSTGRES_ROW_CODEC_MODULES
  ${BIOSEQ_SQL_ROW_CODEC_MODULES})
list(REMOVE_ITEM BIOSEQ_SQL_POSTGRES_ROW_CODEC_MODULES
  ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_ROW_CODECS})
set_source_files_properties(
  ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_PROVIDERS}
  PROPERTIES GENERATED TRUE)

add_library(bioseq_sql_compile_contract_providers OBJECT)
target_sources(bioseq_sql_compile_contract_providers
  PUBLIC
    FILE_SET bioseq_sql_contract_authored_providers
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS}
    FILE_SET bioseq_sql_contract_generated_providers
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES ${BIOSEQ_SQL_COMPILE_CONTRACT_GENERATED_PROVIDERS})
target_include_directories(bioseq_sql_compile_contract_providers PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include"
  "${CMAKE_CURRENT_LIST_DIR}/backend"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/safestringlib/include"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmt"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/seqan3/include")
target_compile_definitions(bioseq_sql_compile_contract_providers PUBLIC
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
target_compile_options(bioseq_sql_compile_contract_providers PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
if(BIOSEQ_TEST_INDEXES)
  target_compile_definitions(bioseq_sql_compile_contract_providers
    PUBLIC BIOSEQ_TEST_INDEXES=1)
endif()
target_link_libraries(bioseq_sql_compile_contract_providers PUBLIC
  bioseq_modules_nopg
  bioseq_pg_support
  bioseq_pg_sql_boundary
  bioseq_sql_annotation_model)
set_target_properties(bioseq_sql_compile_contract_providers PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
# These objects compile only active-profile SUPPORT providers and
# profile-neutral CORE handles. Keep those exact generation prerequisites, but
# do not serialize compilation behind the FINAL SQL validation closures. FINAL
# generation and dual-profile validation remain ordinary DSO/link gates.
add_dependencies(
  bioseq_sql_compile_contract_providers
  bioseq_sql_support_${_bioseq_sql_active_profile}
  bioseq_sql_core_entity_handles)

# A production configure still prepares the independent test_indexes Clang
# profile. Give PREPARE one non-linking compile-command owner for the
# test-profile-only row-codec provider without attaching it to the production
# extension. In a BIOSEQ_TEST_INDEXES build the extension above is the owner,
# so this donor is deliberately absent.
if(NOT BIOSEQ_TEST_INDEXES)
  add_library(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    OBJECT EXCLUDE_FROM_ALL)
  target_sources(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    PRIVATE
      FILE_SET bioseq_sql_row_codec_runtime_test_support_compile_donor_modules
      TYPE CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
      FILES ${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULES})
  target_link_libraries(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    PRIVATE bioseq_sql_compile_contract_providers)
  set_target_properties(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      POSITION_INDEPENDENT_CODE ON
      INTERPROCEDURAL_OPTIMIZATION FALSE)
  add_dependencies(
    bioseq_sql_row_codec_runtime_test_support_compile_donor
    bioseq_sql_compile_contract_providers)
endif()

add_library(bioseq_sql_gcc_compile_contract_objects OBJECT EXCLUDE_FROM_ALL
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})
target_sources(bioseq_sql_gcc_compile_contract_objects
  PRIVATE
    FILE_SET bioseq_sql_gcc_contract_modules
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})
target_include_directories(bioseq_sql_gcc_compile_contract_objects PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include"
  "${CMAKE_CURRENT_LIST_DIR}/backend")
target_link_libraries(bioseq_sql_gcc_compile_contract_objects PRIVATE
  bioseq_sql_compile_contract_providers)
set_target_properties(bioseq_sql_gcc_compile_contract_objects PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
add_dependencies(
  bioseq_sql_gcc_compile_contract_objects
  bioseq_sql_compile_contract_providers)

# Static provenance and two-profile `.core.` descriptor gate for the
# single-compiler compile fixtures above. The ordinary target compiles the
# producer/consumer graph with whichever compiler owns this build; this Python
# step independently rejects authored specializations/access and proves both
# generated profiles describe the same invariant handle modules.
set(BIOSEQ_SQL_GCC_CONTRACT_VERIFIER
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/test_gcc_only_graph.py")
set(BIOSEQ_SQL_GCC_CONTRACT_NEGATIVE_FIXTURES
  ${BIOSEQ_SQL_NEGATIVE_FIXTURES})
set(BIOSEQ_SQL_GCC_CONTRACT_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/gcc-only-compile-contracts.stamp")
file(GLOB_RECURSE BIOSEQ_SQL_GCC_CONTRACT_PROVENANCE_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/backend/*.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/backend/*.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/**/*.cppm")
set(BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS
  "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
  "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/CMakeLists.txt"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/postgres_error_safety.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_codegen.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_sql_codegen.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_gcc_toolchain.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/gcc_hwy_header_unit_launcher.py"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/options.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/fixtures/annotation_probe.cppm"
  ${BIOSEQ_SQL_GCC_CONTRACT_NEGATIVE_FIXTURES}
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES}
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES}
  ${BIOSEQ_SQL_GCC_CONTRACT_PROVENANCE_SOURCES})
set(_bioseq_sql_gcc_contract_generated_args)
foreach(_bioseq_sql_gcc_contract_generated_source IN LISTS
    BIOSEQ_SQL_ENTITY_HANDLE_MODULES)
  list(APPEND _bioseq_sql_gcc_contract_generated_args
    --allowed-core-generated-source
    "${_bioseq_sql_gcc_contract_generated_source}")
endforeach()
foreach(_bioseq_sql_gcc_contract_generated_source IN LISTS
    BIOSEQ_SQL_GUC_production_MODULES)
  list(APPEND _bioseq_sql_gcc_contract_generated_args
    --allowed-production-guc-generated-source
    "${_bioseq_sql_gcc_contract_generated_source}")
endforeach()
foreach(_bioseq_sql_gcc_contract_generated_source IN LISTS
    BIOSEQ_SQL_GUC_test_indexes_MODULES)
  list(APPEND _bioseq_sql_gcc_contract_generated_args
    --allowed-test-indexes-guc-generated-source
    "${_bioseq_sql_gcc_contract_generated_source}")
endforeach()
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_GCC_CONTRACT_STAMP}"
  COMMAND "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_GCC_CONTRACT_VERIFIER}"
    --repository-root "${CMAKE_CURRENT_LIST_DIR}/.."
    --production-manifest
      "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    --test-indexes-manifest
      "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
    --core-manifest "${BIOSEQ_SQL_CORE_MANIFEST}"
    --core-seal "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    --generated-root "${BIOSEQ_SQL_PROFILE_MODULE_ROOT}"
    ${_bioseq_sql_gcc_contract_generated_args}
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_GCC_CONTRACT_STAMP}"
  DEPENDS
    bioseq_sql_final
    "${BIOSEQ_SQL_GCC_CONTRACT_VERIFIER}"
    ${BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS}
    ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
    ${BIOSEQ_SQL_GUC_GENERATED_MODULES}
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
    "${BIOSEQ_SQL_CORE_MANIFEST}"
    "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
  COMMENT
    "Validating GCC-only SQL module/provenance and profile-equal CORE contracts"
  VERBATIM)
add_custom_target(bioseq_sql_gcc_compile_contracts
  DEPENDS "${BIOSEQ_SQL_GCC_CONTRACT_STAMP}")
add_dependencies(
  bioseq_sql_gcc_compile_contracts
  bioseq_sql_gcc_compile_contract_objects
  bioseq_sql_negative_contracts)

# PostgreSQL ERROR-safety is derived from a second, post-FINAL Clang closure.
# FINAL first publishes the generated fmgr entrypoint/registry modules; the
# safety collector then audits those exact modules together with every natural
# FINAL owner.  Each profile owns independent compile, PCM, graph, fact, and
# ledger artifacts so neither profile can discharge the other's obligations.
set(BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/safety")
set(BIOSEQ_SQL_SAFETY_GENERATION_CONTRACT
  "bioseq-postgres-error-safety")
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}")
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_safety_profile_root
    "${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}/${_bioseq_sql_profile}")
  file(MAKE_DIRECTORY "${_bioseq_sql_safety_profile_root}")
  set(_bioseq_sql_safety_sources
    ${BIOSEQ_SQL_REGISTRY_${_bioseq_sql_profile_key}_FINAL_SOURCES}
    ${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINT_MODULES})
  list(SORT _bioseq_sql_safety_sources)
  list(REMOVE_DUPLICATES _bioseq_sql_safety_sources)
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_SOURCE_LIST
    "${_bioseq_sql_safety_profile_root}/sources.txt")
  _bioseq_sql_write_registry_file(
    "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_SOURCE_LIST}"
    ${_bioseq_sql_safety_sources})

  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_DATABASE
    "${_bioseq_sql_safety_profile_root}/clang/compile_commands.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_ROOT
    "${_bioseq_sql_safety_profile_root}/clang/pcm")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_MANIFEST
    "${_bioseq_sql_safety_profile_root}/clang/pcm-manifest.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_DEPFILE
    "${_bioseq_sql_safety_profile_root}/clang/prepare.d")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_STAMP
    "${_bioseq_sql_safety_profile_root}/clang/prepare.stamp")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH
    "${_bioseq_sql_safety_profile_root}/canonical-compile-graph.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS
    "${_bioseq_sql_safety_profile_root}/compiler-facts.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_FACTS_STAMP
    "${_bioseq_sql_safety_profile_root}/facts.stamp")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER
    "${_bioseq_sql_safety_profile_root}/postgres-error-ledger.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER_STAMP
    "${_bioseq_sql_safety_profile_root}/ledger.stamp")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY
    "${_bioseq_sql_safety_profile_root}/runtime-probe-inventory.json")
  set(BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY_STAMP
    "${_bioseq_sql_safety_profile_root}/runtime-probe-inventory.stamp")

  set(_bioseq_sql_safety_facts_target
    "bioseq_sql_postgres_error_safety_facts_${_bioseq_sql_profile}")
  set(_bioseq_sql_safety_profile_target
    "bioseq_sql_postgres_error_safety_${_bioseq_sql_profile}")
  set(_bioseq_sql_safety_inventory_target
    "bioseq_sql_postgres_error_safety_inventory_${_bioseq_sql_profile}")
  bioseq_add_postgres_error_safety_facts_profile(
    TARGET "${_bioseq_sql_safety_facts_target}"
    PROFILE "${_bioseq_sql_profile}"
    FINAL_TARGET "bioseq_sql_final_${_bioseq_sql_profile}"
    SOURCE_LIST
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_SOURCE_LIST}"
    RAW_COMPILE_DATABASE "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    SQL_MANIFEST
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    PROVENANCE "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    OUTPUT_COMPILE_DATABASE
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_DATABASE}"
    OUTPUT_PCM_ROOT
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_ROOT}"
    OUTPUT_PCM_MANIFEST
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PCM_MANIFEST}"
    OUTPUT_PREPARE_DEPFILE
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_DEPFILE}"
    OUTPUT_PREPARE_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_PREPARE_STAMP}"
    OUTPUT_COMPILE_GRAPH
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH}"
    OUTPUT_FACTS
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS}"
    OUTPUT_FACTS_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_FACTS_STAMP}"
    SOURCES ${_bioseq_sql_safety_sources}
    DEPENDS
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}")
  bioseq_add_postgres_error_safety_profile(
    TARGET "${_bioseq_sql_safety_profile_target}"
    PROFILE "${_bioseq_sql_profile}"
    SQL_MANIFEST
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    COMPILER_FACTS
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS}"
    COMPILE_GRAPH
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH}"
    PROVENANCE "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    GENERATOR_REVISION "${BIOSEQ_SQL_SAFETY_GENERATION_CONTRACT}"
    OUTPUT_LEDGER
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER}"
    OUTPUT_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER_STAMP}"
    DEPENDS "${_bioseq_sql_safety_facts_target}")
  bioseq_add_postgres_error_safety_inventory(
    TARGET "${_bioseq_sql_safety_inventory_target}"
    PROFILE "${_bioseq_sql_profile}"
    LEDGER
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_GENERATED_LEDGER}"
    COMPILER_FACTS
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILER_FACTS}"
    COMPILE_GRAPH
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_COMPILE_GRAPH}"
    SQL_MANIFEST
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    PRODUCER_EXECUTABLE "${BIOSEQ_CODEGEN_EXECUTABLE}"
    OUTPUT_INVENTORY
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY}"
    OUTPUT_STAMP
      "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_profile_key}_RUNTIME_INVENTORY_STAMP}"
    DEPENDS "${_bioseq_sql_safety_profile_target}")
endforeach()

bioseq_add_postgres_error_safety_pair(
  TARGET bioseq_sql_postgres_error_safety
  PRODUCTION_TARGET bioseq_sql_postgres_error_safety_production
  TEST_INDEXES_TARGET bioseq_sql_postgres_error_safety_test_indexes)
add_dependencies(
  bioseq_sql_postgres_error_safety
  bioseq_sql_postgres_error_safety_inventory_production
  bioseq_sql_postgres_error_safety_inventory_test_indexes)

set(BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/test_external_feature.py")
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  set(_bioseq_sql_external_feature_expect enabled)
else()
  set(_bioseq_sql_external_feature_expect disabled)
endif()
set(_bioseq_sql_external_feature_contract_stamps)
foreach(_bioseq_sql_profile IN ITEMS production test_indexes)
  string(TOUPPER "${_bioseq_sql_profile}" _bioseq_sql_profile_key)
  set(_bioseq_sql_external_feature_contract_stamp
    "${BIOSEQ_SQL_PROFILE_ARTIFACT_ROOT}/${_bioseq_sql_profile}/external-feature-contract.stamp")
  add_custom_command(
    OUTPUT "${_bioseq_sql_external_feature_contract_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${Python3_EXECUTABLE}"
      "${BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT}"
      --install-sql
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}"
      --manifest
        "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
      --expect "${_bioseq_sql_external_feature_expect}"
      --stamp "${_bioseq_sql_external_feature_contract_stamp}"
    DEPENDS
      "${BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
    COMMENT
      "Verifying ${_bioseq_sql_profile} external SQL feature contract"
    VERBATIM)
  list(APPEND _bioseq_sql_external_feature_contract_stamps
    "${_bioseq_sql_external_feature_contract_stamp}")
endforeach()
add_custom_target(bioseq_sql_external_feature_contract
  DEPENDS ${_bioseq_sql_external_feature_contract_stamps})

set(_bioseq_sql_generation_contract_outputs
  ${_bioseq_sql_final_stamps}
  ${_bioseq_sql_external_feature_contract_stamps})
# The full post-FINAL Clang facts validation is opt-in. Keep its static report
# available without pulling it into ordinary generated-SQL or extension-link
# gates.
if(NOT "${_bioseq_sql_safety_review_inputs}" STREQUAL "")
  set(BIOSEQ_SQL_SAFETY_VALIDATION_REPORT
    "${BIOSEQ_SQL_CODEGEN_DIR}/bioseq-safety-static-validation.json")
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}"
    COMMAND "${Python3_EXECUTABLE}"
      "${BIOSEQ_SQL_VALIDATOR}"
      --static-only
      --production-ledger
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
      --test-indexes-ledger
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
      --reviewed-production-ledger
        "${BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER}"
      --reviewed-test-indexes-ledger
        "${BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER}"
      --production-source-list
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_SOURCE_LIST}"
      --test-indexes-source-list
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_SOURCE_LIST}"
      --production-compile-database
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_DATABASE}"
      --test-indexes-compile-database
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_DATABASE}"
      --production-compile-graph
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_GRAPH}"
      --test-indexes-compile-graph
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_GRAPH}"
      --production-pcm-manifest
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_PCM_MANIFEST}"
      --test-indexes-pcm-manifest
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PCM_MANIFEST}"
      --production-compiler-facts
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILER_FACTS}"
      --test-indexes-compiler-facts
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILER_FACTS}"
      --production-compiler-facts-producer-executable
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
      --test-indexes-compiler-facts-producer-executable
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
      --provenance
        "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
      --production-sql-manifest
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
      --test-indexes-sql-manifest
        "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
      ${_bioseq_sql_completion_args}
      --report "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}"
    DEPENDS
      bioseq_sql_postgres_error_safety
      "${BIOSEQ_SQL_VALIDATOR}"
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_SOURCE_LIST}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_SOURCE_LIST}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_DATABASE}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_DATABASE}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_GRAPH}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_GRAPH}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PCM_MANIFEST}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PCM_MANIFEST}"
      "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILER_FACTS}"
      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILER_FACTS}"
      "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
      "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
      ${_bioseq_sql_safety_review_inputs}
    COMMENT
      "Validating current PostgreSQL ERROR-safety ledgers against reviewed inputs"
    VERBATIM)
  add_custom_target(bioseq_sql_postgres_error_safety_static_evidence
    DEPENDS "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}")
endif()
add_custom_target(bioseq_sql_generation_contract
  DEPENDS ${_bioseq_sql_generation_contract_outputs})
add_dependencies(
  bioseq_sql_generation_contract
  bioseq_sql_validator_self_test
  bioseq_sql_external_feature_contract)
bioseq_add_postgresql_extension(
  postgresql_bioseq
  TYPE POSTGRES_EXTENSION
  CXX 26
  RELOCATABLE
  NAME bioseq
  COMMENT "BioSeqDB PG Extension"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/backend/pg/include"
    "${CMAKE_CURRENT_LIST_DIR}/backend"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/safestringlib/include"
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
    bioseq_sql_compile_contract_providers
  SOURCES
    ${SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
    "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  CXX_MOD_FILES
    ${SRC_MOD_FILES}
  GENERATED_INSTALL
    "${BIOSEQ_SQL_GENERATED_INSTALL}"
  SQL_MANIFEST
    "${BIOSEQ_SQL_GENERATED_MANIFEST}"
  DSO_CLOSURE_CONTRACT manifest
  DSO_ABI_EXPORTS
    Pg_magic_func
    _PG_init
    bioseq_scheduler_supervisor_main
    bioseq_hash_major_parallel_scan_probe_worker_main
  ${_bioseq_sql_completion_target_option}
  PG_ENTRYPOINT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/lifecycle.cppm"
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)
# These scheduler-owned pipeline entries are intentionally absent from the
# unit/integration executable variants created by the helper above.  Link the
# dedicated OBJECT target only into the production PostgreSQL DSO; placing it
# in SHARED_MODULE_LIBS would propagate its objects to every helper variant
# and restore the cross-target COMDAT/interposition hazard this boundary
# removes.
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_pipeline_dispatch_owners)
get_target_property(_bioseq_postgresql_direct_links
  postgresql_bioseq LINK_LIBRARIES)
set(_bioseq_pipeline_dispatch_owner_link_occurrences 0)
foreach(_bioseq_postgresql_direct_link IN LISTS
    _bioseq_postgresql_direct_links)
  if(_bioseq_postgresql_direct_link STREQUAL
      "bioseq_pipeline_dispatch_owners")
    math(EXPR _bioseq_pipeline_dispatch_owner_link_occurrences
      "${_bioseq_pipeline_dispatch_owner_link_occurrences} + 1")
  endif()
endforeach()
if(NOT _bioseq_pipeline_dispatch_owner_link_occurrences EQUAL 1)
  message(FATAL_ERROR
    "postgresql_bioseq must link the pipeline owner OBJECT target exactly "
    "once; found ${_bioseq_pipeline_dispatch_owner_link_occurrences}")
endif()
get_target_property(_bioseq_postgresql_attached_sources
  postgresql_bioseq SOURCES)
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  if(_bioseq_pipeline_dispatch_owner IN_LIST SRC_FILES
      OR _bioseq_pipeline_dispatch_owner IN_LIST SRC_MOD_FILES
      OR _bioseq_pipeline_dispatch_owner IN_LIST
        BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
      OR _bioseq_pipeline_dispatch_owner IN_LIST BIOSEQ_NOPG_SOURCES
      OR _bioseq_pipeline_dispatch_owner IN_LIST
        BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS
      OR _bioseq_pipeline_dispatch_owner IN_LIST BIOSEQ_NOPG_OWNED_MODULES
      OR _bioseq_pipeline_dispatch_owner IN_LIST
        _bioseq_postgresql_attached_sources)
    message(FATAL_ERROR
      "Pipeline owner escaped its dedicated OBJECT target: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner)
unset(_bioseq_postgresql_attached_sources)
unset(_bioseq_pipeline_dispatch_owner_link_occurrences)
unset(_bioseq_postgresql_direct_link)
unset(_bioseq_postgresql_direct_links)
add_dependencies(postgresql_bioseq
  "${BIOSEQ_TYPED_GENERATION_TARGET}")

# Generated target primaries are the only exported providers. Their private
# binder implementation units and cold inventory are ordinary private sources.
# Algorithm partitions and the dispatch-processor registry are named module
# partition providers, so CMake must own them through a CXX_MODULES file set.
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES}
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS}
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS}
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY}"
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PROPERTIES GENERATED TRUE)
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS}
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_sources(postgresql_bioseq
  PRIVATE
    ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS}
    "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PRIVATE FILE_SET bioseq_highway_generated_primaries
    TYPE CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES
      ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES}
      ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS}
      "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY}")
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_highway_fixed_target)
target_link_libraries(bioseq_modules_nopg PRIVATE
  bioseq_highway_target_policy)
add_dependencies(postgresql_bioseq bioseq_highway_target_modules)

set_source_files_properties(
  ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
  ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES}
  ${BIOSEQ_SQL_TEST_INDEXES_ROW_CODEC_MODULES}
  ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES}
  ${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES}
  ${BIOSEQ_SQL_GUC_GENERATED_MODULES}
  PROPERTIES GENERATED TRUE)
target_sources(postgresql_bioseq
  PRIVATE
    FILE_SET bioseq_sql_generated_row_codecs
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES ${BIOSEQ_SQL_POSTGRES_ROW_CODEC_MODULES})
target_sources(postgresql_bioseq
  PRIVATE
    FILE_SET bioseq_sql_generated_fmgr_entrypoints
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES ${BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES})
target_compile_options(postgresql_bioseq PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  target_compile_definitions(
    postgresql_bioseq PRIVATE BIOSEQ_EXTERNAL_PG=1)
  target_compile_definitions(
    bioseq_pipeline_dispatch_owners PRIVATE BIOSEQ_EXTERNAL_PG=1)
endif()
# The Bioseq DSO always owns the provider half of the cache rendezvous ABI and
# the small PG lifecycle helpers, even when the sibling cache DSO is disabled.
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_cache_provider_api
  bioseq_pg_support
  bioseq_pg_sql_boundary
  bioseq_sql_annotation_model)

# Link validation consumes the generator-owned scan/modmap closure before the
# DSO link. A concrete link dependency expresses that ordering without making
# every source scan wait on its own validation.
set(BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/postgresql-bioseq-link-validation.stamp")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CODEGEN_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}"
  DEPENDS
    bioseq_sql_generation_contract
    bioseq_sql_gcc_compile_contracts
  COMMENT
    "Validating SQL generation and module boundaries before linking bioseq"
  VERBATIM)
add_custom_target(bioseq_sql_postgresql_link_proof
  DEPENDS "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}")
set_property(TARGET postgresql_bioseq APPEND PROPERTY LINK_DEPENDS
  "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}")

# Keep only compile-readiness dependencies target-wide. Validation targets belong
# to the link gate above; attaching them here would re-enter CXX.dd through the
# extension's .ddi object-order dependency.
add_dependencies(postgresql_bioseq bioseq_codegen_project)
add_dependencies(postgresql_bioseq bioseq_sql_annotation_probe)

if(TARGET bioseq_gpu_archive_builder)
  add_dependencies(postgresql_bioseq bioseq_gpu_archive_builder)
endif()

# Disable LTO on the PostgreSQL extension. ThinLTO repeats optimization across
# the dispatch-buffer template fan-out and can drop module-attached TLS-init
# wrappers. bioseq_modules_nopg uses the same non-LTO policy, so the extension
# retains consistent linkage and bounded template-instantiation work.
set_target_properties(postgresql_bioseq PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE
# The PostgreSQL extension must not link-load libvulkan in the
  # postmaster, because the macOS loader stack maps Metal/IOAccelerator before
  # backend fork. Keep the vcpkg rpath only so Runtime::init() can later
  # dlopen("@rpath/libvulkan...") inside the selected-GPU backend process.
  INSTALL_RPATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib;${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib")

# Throttle the per-target heavy module compiles on low-RAM hosts. The
# postgresql_bioseq target owns the Domain P module fan-out — the modules
# that import the columnar storage / metadata / table-io stack and feel
# the most BMI-load + template-instantiation pressure (utils.cppm,
# access_method.cppm, custom_scan.cppm, aggregator_node.cppm, …). When
# BIOSEQ_HEAVY_JOBS is set the assignment caps these
# compiles against the same Ninja pool used by bioseq_modules_nopg, so
# the worst-case concurrent peak RSS is `heavy_compile × per-TU peak`
# instead of `min(--parallel, ncpu) × per-TU peak`. Same Ninja-generator
# caveat as the lib's annotation above; pool is unset (no-op) on other
# generators or when the user has not opted in.
if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
  set_target_properties(postgresql_bioseq PROPERTIES
    JOB_POOL_COMPILE heavy_compile)
  if(TARGET bioseq_unit_tests)
    set_target_properties(bioseq_unit_tests PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
  if(TARGET bioseq_integration_tests)
    set_target_properties(bioseq_integration_tests PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
endif()

# Compile the test-only indexing descriptors
# (TestTableCountIndex, a CHUNKED_PER_TABLE index) into the server-loaded
# extension dylib AND the integration/unit test execs. Default OFF (production);
# the integration CI preset sets -DBIOSEQ_TEST_INDEXES=ON so the
# [indexing][per-table] test and the corruption-hook tests run.
if(BIOSEQ_TEST_INDEXES)
  target_compile_definitions(postgresql_bioseq PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(
    bioseq_pipeline_dispatch_owners PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(bioseq_modules_nopg PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(bioseq_testing_support PRIVATE BIOSEQ_TEST_INDEXES=1)
  if(TARGET bioseq_integration_tests)
    target_compile_definitions(bioseq_integration_tests PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
  if(TARGET bioseq_unit_tests)
    target_compile_definitions(bioseq_unit_tests PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
endif()

if(TARGET bioseq_integration_tests)
  add_dependencies(
    bioseq_integration_tests
    postgresql_bioseq)
endif()

# Runtime ERROR validation is an explicit, non-ALL gate. One build tree owns the
# active profile DSO and both profile-specific sealed harnesses; the inverse
# profile DSO must come from a separately configured, runtime-instrumented build
# so the two compilation profiles can never collapse to one binary.
if("${BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE}" STREQUAL "")
  if(NOT "${BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR}" STREQUAL "")
    message(FATAL_ERROR
      "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR requires "
      "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE")
  endif()
  add_custom_target(bioseq_sql_postgres_error_safety_evidence
    COMMAND "${CMAKE_COMMAND}" -E echo
      "Runtime safety validation is not configured. Set "
      "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE to build sealed profile "
      "plans and instrument the active DSO."
    COMMAND "${CMAKE_COMMAND}" -E false
    USES_TERMINAL
    VERBATIM)
else()
  get_filename_component(_bioseq_sql_runtime_case_template
    "${BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE}" ABSOLUTE)
  if(NOT EXISTS "${_bioseq_sql_runtime_case_template}"
     OR IS_DIRECTORY "${_bioseq_sql_runtime_case_template}"
     OR IS_SYMLINK "${_bioseq_sql_runtime_case_template}")
    message(FATAL_ERROR
      "Runtime case template must be an existing regular non-symlink file: "
      "${_bioseq_sql_runtime_case_template}")
  endif()

  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  get_target_property(_bioseq_sql_runtime_control_file
    postgresql_bioseq BIOSEQ_EXTENSION_CONTROL_FILE)
  if(NOT _bioseq_sql_runtime_control_file
     OR _bioseq_sql_runtime_control_file MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
      "postgresql_bioseq did not publish BIOSEQ_EXTENSION_CONTROL_FILE")
  endif()
  if(NOT EXISTS "${_bioseq_sql_runtime_control_file}"
     OR IS_DIRECTORY "${_bioseq_sql_runtime_control_file}"
     OR IS_SYMLINK "${_bioseq_sql_runtime_control_file}")
    message(FATAL_ERROR
      "postgresql_bioseq control file is absent or ambiguous: "
      "${_bioseq_sql_runtime_control_file}")
  endif()

  set(_bioseq_sql_runtime_root
    "${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}/runtime")
  set(_bioseq_sql_runtime_control_target
    bioseq_sql_postgres_error_safety_runtime_control)
  bioseq_add_postgres_error_safety_runtime_control(
    TARGET "${_bioseq_sql_runtime_control_target}"
    SOURCE
      "${CMAKE_CURRENT_LIST_DIR}/tests/integration/support/safety_runtime_control.cppm"
    INCLUDE_DIRS
      "${CMAKE_SOURCE_DIR}"
    LINKS
      PostgreSQL::ServerHeaders
      ${CMAKE_DL_LIBS})

  foreach(_bioseq_sql_runtime_profile IN ITEMS production test_indexes)
    string(TOUPPER "${_bioseq_sql_runtime_profile}"
      _bioseq_sql_runtime_profile_key)
    set(_bioseq_sql_runtime_harness_target
      "bioseq_sql_postgres_error_safety_runtime_${_bioseq_sql_runtime_profile}_harness")
    bioseq_add_postgres_error_safety_runtime_profile(
      TARGET "${_bioseq_sql_runtime_harness_target}"
      PROFILE "${_bioseq_sql_runtime_profile}"
      LEDGER
        "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_runtime_profile_key}_GENERATED_LEDGER}"
      RUNTIME_INVENTORY
        "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_runtime_profile_key}_RUNTIME_INVENTORY}"
      RUNTIME_INVENTORY_PRODUCER_EXECUTABLE
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
      SQL_MANIFEST
        "${BIOSEQ_SQL_${_bioseq_sql_runtime_profile_key}_GENERATED_MANIFEST}"
      SQL_FILE
        "${BIOSEQ_SQL_${_bioseq_sql_runtime_profile_key}_GENERATED_INSTALL}"
      CONTROL_FILE "${_bioseq_sql_runtime_control_file}"
      CONTROL_DSO_TARGET "${_bioseq_sql_runtime_control_target}"
      RUNTIME_POLICY "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
      CASE_TEMPLATE "${_bioseq_sql_runtime_case_template}"
      HARNESS_SOURCE
        "${CMAKE_CURRENT_LIST_DIR}/tests/integration/support/safety_runtime_harness_main.cc"
      OUTPUT_DIRECTORY
        "${_bioseq_sql_runtime_root}/${_bioseq_sql_runtime_profile}"
      LINKS
        fmt
        PostgreSQL::PostgreSQL
        OpenSSL::Crypto
        nlohmann_json_schema_validator::validator
        Threads::Threads
      DEPENDS
        "bioseq_sql_postgres_error_safety_${_bioseq_sql_runtime_profile}"
        "bioseq_sql_postgres_error_safety_inventory_${_bioseq_sql_runtime_profile}"
        "bioseq_sql_final_${_bioseq_sql_runtime_profile}")
    set(_bioseq_sql_runtime_plan_variable
      "${_bioseq_sql_runtime_harness_target}_CASE_PLAN")
    set(_bioseq_sql_runtime_self_test_variable
      "${_bioseq_sql_runtime_harness_target}_SELF_TEST_STAMP")
    set(BIOSEQ_SQL_SAFETY_RUNTIME_${_bioseq_sql_runtime_profile_key}_HARNESS
      "${_bioseq_sql_runtime_harness_target}")
    set(BIOSEQ_SQL_SAFETY_RUNTIME_${_bioseq_sql_runtime_profile_key}_PLAN
      "${${_bioseq_sql_runtime_plan_variable}}")
    set(BIOSEQ_SQL_SAFETY_RUNTIME_${_bioseq_sql_runtime_profile_key}_SELF_TEST
      "${${_bioseq_sql_runtime_self_test_variable}}")
  endforeach()

  if(BIOSEQ_TEST_INDEXES)
    set(_bioseq_sql_runtime_active_profile test_indexes)
  else()
    set(_bioseq_sql_runtime_active_profile production)
  endif()
  target_compile_definitions(postgresql_bioseq PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")
  target_compile_definitions(bioseq_pipeline_dispatch_owners PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")
  target_compile_definitions(bioseq_pg_support PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")
  target_compile_definitions(bioseq_pg_sql_boundary PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")

  add_custom_target(bioseq_sql_postgres_error_safety_runtime_profiles
    DEPENDS
      "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_SELF_TEST}"
      "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_SELF_TEST}")
  add_dependencies(
    bioseq_sql_postgres_error_safety_runtime_profiles
    postgresql_bioseq
    "${_bioseq_sql_runtime_control_target}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}_self_test"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}_self_test")

  if("${BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR}" STREQUAL "")
    add_custom_target(bioseq_sql_postgres_error_safety_evidence
      COMMAND "${CMAKE_COMMAND}" -E echo
        "Sealed profile artifacts are configured, but the live "
        "dual-profile validation gate requires "
        "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR."
      COMMAND "${CMAKE_COMMAND}" -E false
      USES_TERMINAL
      VERBATIM)
  else()
    if(BIOSEQ_TEST_INDEXES)
      message(FATAL_ERROR
        "Aggregate validation must run from the production profile "
        "with a sibling test-index build tree")
    endif()
    if(NOT BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE STREQUAL "isolated")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE must be isolated for "
        "dual-profile validation")
    endif()
    if(NOT BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS
       MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS must be an integer in "
        "the range 1..600")
    elseif(BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS GREATER 600)
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS must be an integer in "
        "the range 1..600")
    endif()
    get_filename_component(_bioseq_sql_runtime_peer_build_dir
      "${BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR}" ABSOLUTE)
    if("${_bioseq_sql_runtime_peer_build_dir}" STREQUAL
       "${CMAKE_BINARY_DIR}")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR must name an isolated "
        "sibling build tree")
    endif()

    # The sibling profile is a build output, so configure must not probe it.
    # The exact expected path becomes a build dependency and fails closed if
    # the separately built test-index tree did not publish that DSO.
    set(_bioseq_sql_runtime_production_dso
      "$<TARGET_FILE:postgresql_bioseq>")
    string(CONCAT _bioseq_sql_runtime_test_indexes_dso
      "${_bioseq_sql_runtime_peer_build_dir}/bioseq/"
      "$<TARGET_FILE_NAME:postgresql_bioseq>")

    set(_bioseq_sql_runtime_lifecycle
      "${CMAKE_CURRENT_LIST_DIR}/../cmake/run_private_postgres_safety_tests.py")
    set(_bioseq_sql_runtime_stage_script
      "${CMAKE_CURRENT_LIST_DIR}/../cmake/stage_private_postgres.cmake")
    set(_bioseq_sql_runtime_config_template
      "${CMAKE_CURRENT_LIST_DIR}/../cmake/private_postgresql.conf.in")
    set(_bioseq_sql_runtime_producer
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/run_postgres_error_safety_tests.py")
    file(REAL_PATH "${CMAKE_COMMAND}"
      _bioseq_sql_runtime_cmake_executable)
    if(NOT EXISTS "${_bioseq_sql_runtime_cmake_executable}"
       OR IS_DIRECTORY "${_bioseq_sql_runtime_cmake_executable}"
       OR IS_SYMLINK "${_bioseq_sql_runtime_cmake_executable}")
      message(FATAL_ERROR
        "Runtime validation requires CMake to resolve to a regular "
        "executable: ${_bioseq_sql_runtime_cmake_executable}")
    endif()
    foreach(_bioseq_sql_runtime_source IN ITEMS
        _bioseq_sql_runtime_lifecycle
        _bioseq_sql_runtime_stage_script
        _bioseq_sql_runtime_config_template
        _bioseq_sql_runtime_producer)
      if(NOT EXISTS "${${_bioseq_sql_runtime_source}}"
         OR IS_DIRECTORY "${${_bioseq_sql_runtime_source}}")
        message(FATAL_ERROR
          "Runtime validation source is missing: "
          "${${_bioseq_sql_runtime_source}}")
      endif()
    endforeach()

    set(_bioseq_sql_runtime_candidate_root
      "${_bioseq_sql_runtime_root}/candidate")
    set(_bioseq_sql_runtime_candidate_production_ledger
      "${_bioseq_sql_runtime_candidate_root}/production-ledger.json")
    set(_bioseq_sql_runtime_candidate_test_indexes_ledger
      "${_bioseq_sql_runtime_candidate_root}/test-indexes-ledger.json")
    set(_bioseq_sql_runtime_candidate_matrix
      "${_bioseq_sql_runtime_candidate_root}/runtime-matrix.json")
    set(_bioseq_sql_runtime_candidate_stamp
      "${_bioseq_sql_runtime_candidate_root}/candidate.stamp")
    set(_bioseq_sql_runtime_candidate_production_stage_manifest
      "${_bioseq_sql_runtime_candidate_root}/stages/production/stage-manifest.json")
    set(_bioseq_sql_runtime_candidate_test_indexes_stage_manifest
      "${_bioseq_sql_runtime_candidate_root}/stages/test_indexes/stage-manifest.json")
    set(_bioseq_sql_runtime_reviewed_root
      "${_bioseq_sql_runtime_candidate_root}/reviewed")
    set(_bioseq_sql_runtime_validation_report
      "${_bioseq_sql_runtime_candidate_root}/runtime-validation.json")

    add_custom_command(
      OUTPUT
        "${_bioseq_sql_runtime_candidate_production_ledger}"
        "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        "${_bioseq_sql_runtime_candidate_matrix}"
        "${_bioseq_sql_runtime_candidate_stamp}"
        "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}"
      COMMAND "${_bioseq_sql_runtime_cmake_executable}" -E env
        "PYTHONDONTWRITEBYTECODE=1"
        "${Python3_EXECUTABLE}"
        "${_bioseq_sql_runtime_lifecycle}"
        --cmake "${_bioseq_sql_runtime_cmake_executable}"
        --pg-config "${PostgreSQL_CONFIG}"
        --stage-script "${_bioseq_sql_runtime_stage_script}"
        --config-template "${_bioseq_sql_runtime_config_template}"
        --runtime-producer "${_bioseq_sql_runtime_producer}"
        --production-ledger
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
        --test-indexes-ledger
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
        --production-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_RUNTIME_INVENTORY}"
        --test-indexes-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_RUNTIME_INVENTORY}"
        --runtime-inventory-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --production-sql-manifest
          "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
        --test-indexes-sql-manifest
          "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
        --production-sql-file
          "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
        --test-indexes-sql-file
          "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_INSTALL}"
        --control-file "${_bioseq_sql_runtime_control_file}"
        --control-dso "$<TARGET_FILE:${_bioseq_sql_runtime_control_target}>"
        --runtime-policy "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
        --production-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_PLAN}"
        --test-indexes-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_PLAN}"
        --production-dso "${_bioseq_sql_runtime_production_dso}"
        --test-indexes-dso "${_bioseq_sql_runtime_test_indexes_dso}"
        --production-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}>"
        --test-indexes-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}>"
        --output-root "${_bioseq_sql_runtime_candidate_root}"
        --stamp "${_bioseq_sql_runtime_candidate_stamp}"
        --timeout-seconds
          "${BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS}"
      DEPENDS
        bioseq_sql_postgres_error_safety_runtime_profiles
        postgresql_bioseq
        "${_bioseq_sql_runtime_control_target}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}"
        "${_bioseq_sql_runtime_lifecycle}"
        "${_bioseq_sql_runtime_stage_script}"
        "${_bioseq_sql_runtime_config_template}"
        "${_bioseq_sql_runtime_producer}"
        "${_bioseq_sql_runtime_cmake_executable}"
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_RUNTIME_INVENTORY}"
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_RUNTIME_INVENTORY}"
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
        "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
        "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_INSTALL}"
        "${_bioseq_sql_runtime_control_file}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_PLAN}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_PLAN}"
        "${_bioseq_sql_runtime_test_indexes_dso}"
      COMMENT
        "Publishing dual-profile PostgreSQL ERROR runtime validation candidate"
      USES_TERMINAL
      VERBATIM)

    add_custom_target(bioseq_sql_postgres_error_safety_candidate
      DEPENDS
        "${_bioseq_sql_runtime_candidate_production_ledger}"
        "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        "${_bioseq_sql_runtime_candidate_matrix}"
        "${_bioseq_sql_runtime_candidate_stamp}"
        "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}")

    add_custom_target(bioseq_sql_postgres_error_safety_evidence
      BYPRODUCTS "${_bioseq_sql_runtime_validation_report}"
      COMMAND "${_bioseq_sql_runtime_cmake_executable}" -E rm -f
        "${_bioseq_sql_runtime_validation_report}"
      COMMAND "${_bioseq_sql_runtime_cmake_executable}" -E env
        "PYTHONDONTWRITEBYTECODE=1"
        "${Python3_EXECUTABLE}"
        "${BIOSEQ_SQL_VALIDATOR}"
        --evidence-only
        --require-complete-codegen
        --production-ledger
          "${_bioseq_sql_runtime_candidate_production_ledger}"
        --test-indexes-ledger
          "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        --reviewed-production-ledger
          "${_bioseq_sql_runtime_reviewed_root}/production-ledger.json"
        --reviewed-test-indexes-ledger
          "${_bioseq_sql_runtime_reviewed_root}/test-indexes-ledger.json"
        --production-source-list
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_SOURCE_LIST}"
        --test-indexes-source-list
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_SOURCE_LIST}"
        --production-compile-database
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_DATABASE}"
        --test-indexes-compile-database
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_DATABASE}"
        --production-compile-graph
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_GRAPH}"
        --test-indexes-compile-graph
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_GRAPH}"
        --production-pcm-manifest
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_PCM_MANIFEST}"
        --test-indexes-pcm-manifest
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PCM_MANIFEST}"
        --production-compiler-facts
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILER_FACTS}"
        --test-indexes-compiler-facts
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILER_FACTS}"
        --production-compiler-facts-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --test-indexes-compiler-facts-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --provenance "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
        --production-sql-manifest
          "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
        --test-indexes-sql-manifest
          "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
        --runtime-policy "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
        --runtime-matrix "${_bioseq_sql_runtime_candidate_matrix}"
        --reviewed-runtime-matrix
          "${_bioseq_sql_runtime_reviewed_root}/runtime-matrix.json"
        --production-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_PLAN}"
        --test-indexes-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_PLAN}"
        --production-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_RUNTIME_INVENTORY}"
        --test-indexes-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_RUNTIME_INVENTORY}"
        --runtime-inventory-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --production-profile-dso
          "${_bioseq_sql_runtime_production_dso}"
        --test-indexes-profile-dso
          "${_bioseq_sql_runtime_test_indexes_dso}"
        --production-stage-manifest
          "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        --test-indexes-stage-manifest
          "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}"
        --runtime-control-dso
          "$<TARGET_FILE:${_bioseq_sql_runtime_control_target}>"
        --runtime-producer-executable
          "${_bioseq_sql_runtime_producer}"
        --production-runtime-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}>"
        --test-indexes-runtime-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}>"
        --report "${_bioseq_sql_runtime_validation_report}"
      DEPENDS
        "${_bioseq_sql_runtime_candidate_production_ledger}"
        "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        "${_bioseq_sql_runtime_candidate_matrix}"
        "${_bioseq_sql_runtime_candidate_stamp}"
        "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}"
        "${_bioseq_sql_runtime_cmake_executable}"
      COMMENT
        "Validating promoted dual-profile PostgreSQL ERROR runtime results"
      USES_TERMINAL
      VERBATIM)
  endif()
endif()

# bench_framework — generic, algorithm-agnostic benchmark orchestration.
include("${CMAKE_CURRENT_LIST_DIR}/../bench_framework/build.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/io/build.cmake")

# Primary alignment benchmark, built on top of bench_framework.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/alignment_benchmark.cmake")

# End-to-end SQL benchmark, driving PostgreSQL over libpq
# (bioseq.load + mmsearch) against minimap2.
include("${CMAKE_CURRENT_LIST_DIR}/../bench/bench_e2e.cmake")

# External API descriptors are build-time inputs. Keep their generated clients
# outside BIOSEQ_NOPG_MODULES so changing an OpenAPI schema does not rebuild the
# shared Domain-N module library. The dedicated build mode enables this option;
# normal configurations must not require the host's code-generation toolchain.
# BIOSEQ_BUILD_EXTERNAL_API_CLIENTS is declared at the repository root so the
# isolated generator can be provisioned once before add_subdirectory(bioseq).
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  set(BIOSEQ_EXTERNAL_GENERATED_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/external")
  add_subdirectory(external)
  if(TARGET bioseq_external_api_integration_tests)
    add_dependencies(
      bioseq_external_api_integration_tests
      bioseq_sql_external_feature_contract)
  endif()
  if(TARGET bioseq_external_codegen)
    add_dependencies(
      bioseq_sql_prepare_final_production
      bioseq_external_codegen)
    add_dependencies(
      bioseq_sql_prepare_final_test_indexes
      bioseq_external_codegen)
  endif()

  # Link the compile-time provider pack into the extension so the external
  # CustomScan and row writer resolve bioseq.external.providers.run
  # and the provider model, and expose those module BMIs to the extension's
  # module compiles. BIOSEQ_EXTERNAL_PG gates the _PG_init registration and the
  # conditional import of the scan module in backend/pg/column/index.cppm.
  if(TARGET bioseq_external_provider_adapters)
    # The PG-free streaming library is the single compiled owner of the
    # transport/archive module providers. Linking that owner publishes its
    # module file set to CustomScan while retaining its private crypto edge.
    target_link_libraries(postgresql_bioseq PRIVATE
      bioseq_external_provider_adapters
      bioseq_external_io)
  endif()
endif()

# No executable or reusable library in this directory may acquire the
# scheduler-only owner objects. The single allowed consumer is the PostgreSQL
# DSO; the owner target itself is excluded from the source-leak check.
get_property(_bioseq_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_bioseq_directory_target IN LISTS _bioseq_directory_targets)
  if(_bioseq_directory_target STREQUAL "postgresql_bioseq"
      OR _bioseq_directory_target STREQUAL
        "bioseq_pipeline_dispatch_owners")
    continue()
  endif()
  get_target_property(_bioseq_directory_target_links
    ${_bioseq_directory_target} LINK_LIBRARIES)
  foreach(_bioseq_directory_target_link IN LISTS
      _bioseq_directory_target_links)
    string(FIND "${_bioseq_directory_target_link}"
      "bioseq_pipeline_dispatch_owners"
      _bioseq_pipeline_dispatch_owner_link_offset)
    if(NOT _bioseq_pipeline_dispatch_owner_link_offset EQUAL -1)
      message(FATAL_ERROR
        "Scheduler-only pipeline owner target leaked into "
        "${_bioseq_directory_target}")
    endif()
  endforeach()
  get_target_property(_bioseq_directory_target_sources
    ${_bioseq_directory_target} SOURCES)
  foreach(_bioseq_pipeline_dispatch_owner IN LISTS
      BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
    if(_bioseq_pipeline_dispatch_owner IN_LIST
        _bioseq_directory_target_sources)
      message(FATAL_ERROR
        "Scheduler-only pipeline owner source leaked into "
        "${_bioseq_directory_target}: ${_bioseq_pipeline_dispatch_owner}")
    endif()
  endforeach()
endforeach()
unset(_bioseq_pipeline_dispatch_owner)
unset(_bioseq_directory_target_sources)
unset(_bioseq_pipeline_dispatch_owner_link_offset)
unset(_bioseq_directory_target_link)
unset(_bioseq_directory_target_links)
unset(_bioseq_directory_target)
unset(_bioseq_directory_targets)
