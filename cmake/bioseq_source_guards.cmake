# PostgreSQL must publish the digest-derived MoltenVK environment and install
# its shader provider before the process-local Vulkan Runtime singleton is
# constructed.  Make bypasses fail at configure time, including calls hidden
# in implementation include files instantiated by multiple typed TUs.
file(GLOB_RECURSE BIOSEQ_PG_RUNTIME_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/**/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/*.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/**/*.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/**/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/**/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/**/*.hpp)
set(BIOSEQ_PG_RUNTIME_OWNER_ALLOWLIST
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/shader_runtime.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/supervisor/vulkan.cppm)
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
# reach through the facade into a concrete scanner/range implementation. These
# files are the complete implementation-owner allowlist:
#   * lazy_sequence_parts/{owners,adapters}.cppm own the facade adapters;
#   * backend/storage/scanner.cppm owns concrete scanner implementations.
# Any new concrete dependency outside those ownership boundaries fails CMake
# configuration, making the allowlist a repository-enforced invariant.
set(BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/storage/lazy_sequence_parts/owners.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/storage/lazy_sequence_parts/adapters.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/storage/scanner.cppm)

# Named unit fixtures may deliberately construct concrete scanners to exercise
# the representation-erasure boundary.  Keep this list explicit: a new test is
# not implicitly exempt merely because its filename ends in `_test.cpp`.
set(BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/alignment/block_aligner_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/boundary_parity_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/fragment_producer/replay_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap/fragment_producer_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_hashmap_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/minimizer_sequence_index_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/indexing/symbol_counts_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/lazy_minimizer_parity_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/two_stack_exact_checked_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_index_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_segment_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kmers/vulkan_minimizers/vulkan_minimizer_upload_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/batch_sort_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/storage/lazy_sequence_test.cpp)
file(GLOB_RECURSE BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/*.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/**/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/**/*.cpp)
list(REMOVE_DUPLICATES BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES)
list(SORT BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES)

set(_bioseq_lazy_all_allowlisted_sources
  ${BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST}
  ${BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST})
list(LENGTH _bioseq_lazy_all_allowlisted_sources
  _bioseq_lazy_allowlisted_source_count)
list(REMOVE_DUPLICATES _bioseq_lazy_all_allowlisted_sources)
list(LENGTH _bioseq_lazy_all_allowlisted_sources
  _bioseq_lazy_unique_allowlisted_source_count)
if(NOT _bioseq_lazy_allowlisted_source_count EQUAL
    _bioseq_lazy_unique_allowlisted_source_count)
  message(FATAL_ERROR
    "Lazy-concrete allowlists must not contain duplicate source paths")
endif()

foreach(_bioseq_lazy_boundary_source IN LISTS
    BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST)
  file(RELATIVE_PATH _bioseq_lazy_boundary_relative
    "${CMAKE_CURRENT_SOURCE_DIR}" "${_bioseq_lazy_boundary_source}")
  string(REPLACE "\\" "/" _bioseq_lazy_boundary_relative
    "${_bioseq_lazy_boundary_relative}")
  if(NOT _bioseq_lazy_boundary_relative MATCHES
      "^backend/.+\\.cppm$")
    message(FATAL_ERROR
      "Lazy-concrete production owner must be one exact backend .cppm: "
      "${_bioseq_lazy_boundary_source}")
  endif()
endforeach()
foreach(_bioseq_lazy_fixture_source IN LISTS
    BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST)
  file(RELATIVE_PATH _bioseq_lazy_fixture_relative
    "${CMAKE_CURRENT_SOURCE_DIR}" "${_bioseq_lazy_fixture_source}")
  string(REPLACE "\\" "/" _bioseq_lazy_fixture_relative
    "${_bioseq_lazy_fixture_relative}")
  if(NOT _bioseq_lazy_fixture_relative MATCHES
      "^tests/unit/.+_test\\.cpp$")
    message(FATAL_ERROR
      "Lazy-concrete fixture must be one exact unit-test source: "
      "${_bioseq_lazy_fixture_source}")
  endif()
endforeach()
foreach(_bioseq_lazy_allowlisted_source IN LISTS
    _bioseq_lazy_all_allowlisted_sources)
  if(NOT EXISTS "${_bioseq_lazy_allowlisted_source}"
      OR IS_DIRECTORY "${_bioseq_lazy_allowlisted_source}")
    message(FATAL_ERROR
      "Lazy-concrete allowlisted source does not exist: "
      "${_bioseq_lazy_allowlisted_source}")
  endif()
  list(FIND BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES
    "${_bioseq_lazy_allowlisted_source}"
    _bioseq_lazy_allowlisted_inventory_index)
  if(_bioseq_lazy_allowlisted_inventory_index EQUAL -1)
    message(FATAL_ERROR
      "Lazy-concrete allowlisted source is outside the guarded inventory: "
      "${_bioseq_lazy_allowlisted_source}")
  endif()
endforeach()

string(CONCAT _bioseq_lazy_concrete_pattern
  "(\\.scanner[ \t]*\\("
  "|\\.underlying[ \t]*\\("
  "|(^|[^A-Za-z0-9_])lazy_sequence[ \t]*<"
  "|chunked_span_sequence_scanner[ \t]*<)")
set(_bioseq_lazy_matched_production_sources)
set(_bioseq_lazy_matched_fixture_sources)
foreach(_bioseq_lazy_source IN LISTS BIOSEQ_LAZY_BOUNDARY_GUARD_SOURCES)
  file(STRINGS "${_bioseq_lazy_source}" _bioseq_lazy_source_lines)
  set(_bioseq_lazy_source_first_match "")
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
      if(_bioseq_lazy_source_first_match STREQUAL "")
        set(_bioseq_lazy_source_first_match "${_bioseq_lazy_code_line}")
      endif()
    endif()
  endforeach()
  if(_bioseq_lazy_source_first_match STREQUAL "")
    continue()
  endif()

  list(FIND BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST
    "${_bioseq_lazy_source}" _bioseq_lazy_allowlist_index)
  list(FIND BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
    "${_bioseq_lazy_source}" _bioseq_lazy_fixture_index)
  if(NOT _bioseq_lazy_allowlist_index EQUAL -1)
    list(APPEND _bioseq_lazy_matched_production_sources
      "${_bioseq_lazy_source}")
  elseif(NOT _bioseq_lazy_fixture_index EQUAL -1)
    list(APPEND _bioseq_lazy_matched_fixture_sources
      "${_bioseq_lazy_source}")
  else()
    message(FATAL_ERROR
      "Lazy-facade representation-erasure violation in "
      "${_bioseq_lazy_source}; consume lazy_sequence_view or update the "
      "documented ownership boundary; matched code: "
      "${_bioseq_lazy_source_first_match}")
  endif()
endforeach()

set(_bioseq_lazy_expected_production_sources
  ${BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST})
set(_bioseq_lazy_expected_fixture_sources
  ${BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST})
list(SORT _bioseq_lazy_expected_production_sources)
list(SORT _bioseq_lazy_expected_fixture_sources)
list(SORT _bioseq_lazy_matched_production_sources)
list(SORT _bioseq_lazy_matched_fixture_sources)
if(NOT "${_bioseq_lazy_matched_production_sources}" STREQUAL
    "${_bioseq_lazy_expected_production_sources}")
  message(FATAL_ERROR
    "Lazy-concrete production-owner allowlist contains a source without a "
    "guarded construct: expected '${_bioseq_lazy_expected_production_sources}', "
    "matched '${_bioseq_lazy_matched_production_sources}'")
endif()
if(NOT "${_bioseq_lazy_matched_fixture_sources}" STREQUAL
    "${_bioseq_lazy_expected_fixture_sources}")
  message(FATAL_ERROR
    "Lazy-concrete unit-fixture allowlist contains a source without a guarded "
    "construct: expected '${_bioseq_lazy_expected_fixture_sources}', matched "
    "'${_bioseq_lazy_matched_fixture_sources}'")
endif()

unset(_bioseq_lazy_concrete_pattern)
unset(_bioseq_lazy_all_allowlisted_sources)
unset(_bioseq_lazy_allowlisted_source_count)
unset(_bioseq_lazy_unique_allowlisted_source_count)
unset(_bioseq_lazy_boundary_source)
unset(_bioseq_lazy_boundary_relative)
unset(_bioseq_lazy_fixture_source)
unset(_bioseq_lazy_fixture_relative)
unset(_bioseq_lazy_allowlisted_source)
unset(_bioseq_lazy_allowlisted_inventory_index)
unset(_bioseq_lazy_source)
unset(_bioseq_lazy_source_lines)
unset(_bioseq_lazy_source_line)
unset(_bioseq_lazy_code_line)
unset(_bioseq_lazy_line_comment_offset)
unset(_bioseq_lazy_source_first_match)
unset(_bioseq_lazy_allowlist_index)
unset(_bioseq_lazy_fixture_index)
unset(_bioseq_lazy_expected_production_sources)
unset(_bioseq_lazy_expected_fixture_sources)
unset(_bioseq_lazy_matched_production_sources)
unset(_bioseq_lazy_matched_fixture_sources)

# Lazy-facade completion gate. The broad boundary gate above prevents a
# concrete lazy/scanner type from escaping anywhere in backend code; this
# narrower production-owner sweep also rejects the physical-geometry escape
# hatches named by the specification.
set(BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/stage_graph.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/minimizer_execution.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/minimizer_request_admission.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/minimizer_scalar_highway.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/minimizer_vulkan.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/minimizer_projection.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/execution.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/execution_plan_context.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/execution_scalar_highway.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/execution_vulkan.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/execution_results.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/shape_dispatch.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/entry_points.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/segmented.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/segmented_planning.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/segmented_finalization.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_parts/segmented_execution.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/pipe_owners_template.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/minimap_port.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/minimap_port_parts/results.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/minimap_port_parts/seeding_chaining.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/minimap_port_parts/alignment.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/minimap_port_parts/postprocessing.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pipeline/minimap_port_parts/execution_stats.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/ksw2_kernel.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_dp.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_certificates.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_minimizers.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/state.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/resident_stream.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_vectors.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_decode.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_k15_w10.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_chronological.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/continuation.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/drivers.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/contract.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/upload.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/memory.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/sketch_dispatch.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_materialization.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/batch.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_routes.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/segmented.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/entry_points.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/storage/batch_sort.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_processor/cpu_preparation.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_processor/typed_template.cppm)
file(GLOB_RECURSE BIOSEQ_LAZY_FACADE_INDEX_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/indexing/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/indexing/*.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/indexing/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/indexing/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/indexing/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/*.cppm
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/storage/indexing/*.hpp)
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
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm")
    set(_bioseq_scopes
      bounded_u32_dna4_view
      run_packed_kernel
      collect_minimizers_bounded_nthash_simd_checked)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/resident_stream.cppm")
    set(_bioseq_scopes
      two_stack_simd_iteration_resident_kernel)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_decode.cppm")
    set(_bioseq_scopes
      decode_packed_dna4_rank_vector
      decode_packed_nt4_rank_vector)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/continuation.cppm")
    set(_bioseq_scopes
      reset_two_stack_nthash
      consume_two_stack_nthash_packed_fragment
      consume_two_stack_nthash_fragment)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/drivers.cppm")
    set(_bioseq_scopes
      collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked
      run_fragmented_impl)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/upload.cppm")
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
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_routes.cppm")
    set(_bioseq_scopes
      build_gpu_segment_index_on_device_resident_impl)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/segmented.cppm")
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
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm")
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
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/resident_stream.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_decode.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/continuation.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/drivers.cppm")
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
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/upload.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/index_routes.cppm"
      OR "${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/vulkan_minimizers_parts/segmented.cppm")
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
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm")
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
