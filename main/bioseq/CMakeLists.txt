# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS ${VCPKG_CMAKE_CONFIGURE_OPTIONS} -DSQLGEN_POSTGRES=1)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
option(BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST
  "Run the maintainer PREPARE incremental Ninja contract during the build" ON)
option(BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST
  "Run the maintainer SQL negative/provenance fixture suite during the build" ON)
option(BIOSEQ_DOC0388_RUN_LEDGER_MAINTAINER_CHECK
  "Run the Git-history-backed DOC0388 ledger maintainer check during the build" ON)

set(BIOSEQ_PG_BUILD_CONFIG_HEADER
  "${CMAKE_CURRENT_LIST_DIR}/include2/headers/pg_build_config.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${BIOSEQ_PG_BUILD_CONFIG_HEADER}")
file(STRINGS "${BIOSEQ_PG_BUILD_CONFIG_HEADER}"
  _bioseq_opttimization_level_line
  REGEX "^#define BIOSEQ_OPTTIMIZATION_LEVEL [0-5]$")
list(LENGTH _bioseq_opttimization_level_line
  _bioseq_opttimization_level_line_count)
if(NOT _bioseq_opttimization_level_line_count EQUAL 1)
  message(FATAL_ERROR
    "${BIOSEQ_PG_BUILD_CONFIG_HEADER} must contain exactly one literal "
    "'#define BIOSEQ_OPTTIMIZATION_LEVEL N' with N in 0..5")
endif()
string(REGEX REPLACE
  "^#define BIOSEQ_OPTTIMIZATION_LEVEL ([0-5])$" "\\1"
  BIOSEQ_DEFAULT_OPTTIMIZATION_LEVEL
  "${_bioseq_opttimization_level_line}")
if(NOT BIOSEQ_DEFAULT_OPTTIMIZATION_LEVEL MATCHES "^[0-5]$")
  message(FATAL_ERROR
    "default BIOSEQ_OPTTIMIZATION_LEVEL must be an integer in the range "
    "0..5; got '${BIOSEQ_DEFAULT_OPTTIMIZATION_LEVEL}'")
endif()

# Keep the checked-in header literal as the everyday source of truth while
# allowing isolated qualification presets to compile the complete profile in
# a separate build tree.  The override is deliberately a distinct cache key:
# passing -DBIOSEQ_OPTTIMIZATION_LEVEL directly would make the CMake source
# graph and the preprocessor disagree.
set(BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE "" CACHE STRING
  "Qualification-only BIOSEQ_OPTTIMIZATION_LEVEL override (empty or 0..5)")
set_property(CACHE BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE PROPERTY
  STRINGS "" 0 1 2 3 4 5)
if(NOT "${BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE}" STREQUAL ""
   AND NOT BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE MATCHES "^[0-5]$")
  message(FATAL_ERROR
    "BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE must be empty or an integer in "
    "the range 0..5; got '${BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE}'")
endif()

if("${BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE}" STREQUAL "")
  set(BIOSEQ_OPTTIMIZATION_LEVEL
    "${BIOSEQ_DEFAULT_OPTTIMIZATION_LEVEL}")
  set(_bioseq_opttimization_level_source "pg_build_config.h default")
else()
  set(BIOSEQ_OPTTIMIZATION_LEVEL
    "${BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE}")
  set(_bioseq_opttimization_level_source "qualification preset override")
  add_compile_definitions(
    BIOSEQ_OPTTIMIZATION_LEVEL=${BIOSEQ_OPTTIMIZATION_LEVEL})
endif()
message(STATUS
  "bioseq: BIOSEQ_OPTTIMIZATION_LEVEL=${BIOSEQ_OPTTIMIZATION_LEVEL} "
  "(${_bioseq_opttimization_level_source}; checked-in default="
  "${BIOSEQ_DEFAULT_OPTTIMIZATION_LEVEL})")

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
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor_pg_impl.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor_pg_vulkan.cppm)
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
#   * lazy_compact_sequence.cppm owns the canonical facade adapters;
#   * sequence_scanner*.cppm own scanner implementations and PG leases.
# Any new concrete dependency outside those ownership boundaries fails CMake
# configuration, making the allowlist a repository-enforced invariant.
set(BIOSEQ_LAZY_CONCRETE_BOUNDARY_ALLOWLIST
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_compact_sequence.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/sequence_scanner.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/sequence_scanner_pg.cppm)

# Named unit fixtures may deliberately construct concrete scanners to exercise
# the representation-erasure boundary.  Keep this list explicit: a new test is
# not implicitly exempt merely because its filename ends in `_test.cpp`.
set(BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/block_aligner_reference_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipeline_pipe_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/gpu_sort_oracle_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/boundary_parity_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_hashmap_index_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_sequence_index_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/symbol_counts_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/lazy_minimizer_parity_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimizers_vulkan_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_aligner_parity_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_compact_sequence_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_pipeline_parity_test.cpp)
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
  ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.hpp)
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
        "(\\.scanner[ \t]*\\(|\\.underlying[ \t]*\\(|lazy_compact_sequence[ \t]*<|sequence_scanner_pg[ \t]*<)")
      message(FATAL_ERROR
        "Lazy-facade representation-erasure violation in ${_bioseq_lazy_source}; "
        "consume basic_lazy_sequence_view or update the documented ownership boundary; "
        "matched code: ${_bioseq_lazy_code_line}")
    endif()
  endforeach()
endforeach()

# Lazy-facade completion gate. The broad boundary gate above prevents a
# concrete lazy/scanner type from escaping anywhere in backend code; this
# narrower production-owner sweep also rejects the physical-geometry escape
# hatches named by the specification.
set(BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/paper_nthash_kernel.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimizers_vulkan.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_compact_sequence_batch_processor.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_cpu_preparation.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_typed_template.cppm)
file(GLOB_RECURSE BIOSEQ_LAZY_FACADE_INDEX_GUARD_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/*.hpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/indexing/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/indexing/*.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/indexing/*.inc
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/indexing/*.h
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/indexing/*.hpp)
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
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/paper_nthash_kernel.cppm")
    set(_bioseq_scopes
      bounded_u32_dna4_view
      run_packed_kernel
      collect_minimizers_packed_paper_nthash_simd_checked)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm")
    set(_bioseq_scopes
      consume_two_stack_nthash_fragment
      collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked
      run_fragmented_impl
      scalable_two_stack_storage_fallback)
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimizers_vulkan.cppm")
    set(_bioseq_scopes
      byte_upload_range_address_adapter
      vulkan_sketchable_sequence
      compute_segment_descriptor
      make_phase_c_segment_window
      phase_c_segment_windows
      make_upload_view_from_window
      make_sequence_upload_view
      build_gpu_segment_index_on_device_resident_impl
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
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/paper_nthash_kernel.cppm")
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
        "collect_minimizers_packed_paper_nthash_simd_checked"
        AND ("${_bioseq_line}" STREQUAL "traits::letters_per_unit;"
          OR "${_bioseq_line}" STREQUAL "sequence.read_cptr(begin));"))
      set(_bioseq_allowed TRUE)
    endif()
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm")
    if("${_bioseq_scope}" STREQUAL "consume_two_stack_nthash_fragment"
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
    elseif("${_bioseq_scope}" STREQUAL
        "scalable_two_stack_storage_fallback"
        AND "${_bioseq_line}" STREQUAL
          "constexpr std::size_t letters_per_unit = traits::letters_per_unit;")
      set(_bioseq_allowed TRUE)
    endif()
  elseif("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimizers_vulkan.cppm")
    if("${_bioseq_scope}" STREQUAL "byte_upload_range_address_adapter"
        AND "${_bioseq_line}" STREQUAL
          "[[nodiscard]] const std::byte *read_cptr(const iterator &it) const noexcept {")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "vulkan_sketchable_sequence"
        AND ("${_bioseq_line}" STREQUAL "{ seq.padding() };"
          OR "${_bioseq_line}" STREQUAL "{ seq.read_cptr(seq.cbegin()) };"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "compute_segment_descriptor"
        AND ("${_bioseq_line}" STREQUAL
          "constexpr std::size_t letters_per_unit = traits::letters_per_unit;"
          OR "${_bioseq_line}" STREQUAL
            "? static_cast<std::uint32_t>(seq.padding())"))
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "make_phase_c_segment_window"
        AND "${_bioseq_line}" STREQUAL
          "const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);")
      set(_bioseq_allowed TRUE)
    elseif("${_bioseq_scope}" STREQUAL "phase_c_segment_windows"
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
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/paper_nthash_kernel.cppm")
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

# DOC0304 Phase A — Vulkan/MoltenVK + glslang for the G3SA tiled-DP chaining
# backend. CMake's FindVulkan only materialises Vulkan::glslang when the glslang
# component is requested, so it is listed explicitly. DXC is intentionally NOT
# required: the Phase A shader is GLSL compiled through glslang, and hard-
# requiring DXC would break otherwise-valid MoltenVK / non-Windows configs.
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

# DOC0398 generator inputs are authored module interfaces only so Clang can
# prove their namespace/callable structure. They are never final providers:
# bioseq_codegen emits one uniquely named production module graph per enabled
# Highway target. Keep this explicit list out of both ordinary module globs.
set(BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_utils.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_vector_stream.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/highway_sequence_ops.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_base_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_lexic_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_minimap_int_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/reference_minimap_int_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/paper_nthash_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/utils_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/lane_helpers_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/prefilters/sneaky_snake_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_kernel.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/highway_indexing_kernels.cppm")

# DOC0398 template-flatten inputs have the same exact-set ownership contract as
# Highway target templates. A marked source is scan-only generator input, never
# an ordinary module provider, and every registered input must carry the marker.
set(BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_typed_template.cppm")

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
      "DOC0398 production module units may not textually include another "
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
      "DOC0398 Highway generator input does not exist: "
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
      "DOC0398 Highway generator inputs must be authored beneath "
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
    "DOC0398 [[bioseq::simd]] ownership registry is not an exact set.\n"
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
      "DOC0398 template-flatten generator input does not exist: "
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
      "DOC0398 template-flatten inputs must be authored beneath "
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
      "DOC0398 Highway and template-flatten generator source sets must be "
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
    "DOC0398 [[bioseq::template_flatten]] ownership registry is not an exact set.\n"
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

set(BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_dispatch_contract_impl.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/sorting/index_impl.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_hashmap_index_impl.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  ${BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES}
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS})
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES}
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS})

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
# link these leaves without inheriting the sequence graph. Checked arithmetic
# is owned tree-wide by common/checked_arithmetic and is therefore not part of
# the Bioseq source glob.
set(BIOSEQ_SINK_CONTROL_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/sink_control.cppm")
# DOC0372 §369-374 — read_result + io_source protocol as a lightweight leaf so
# external transfer / generated clients depend on the byte-source protocol
# without the sequences/alphabets graph. The io/*.cppm glob captures it, so it
# must leave BIOSEQ_NOPG_OWNED_MODULES to be compiled exactly once in its target.
set(BIOSEQ_IO_SOURCE_CORE_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/io/source_core.cppm")
list(REMOVE_ITEM BIOSEQ_NOPG_OWNED_MODULES
  "${BIOSEQ_SINK_CONTROL_MODULE}"
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

add_library(bioseq_io_source_core STATIC)
set_target_properties(bioseq_io_source_core PROPERTIES
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
  CXX_MODULE_STD ON POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_io_source_core PRIVATE NO_POSTGRES=1)
target_sources(bioseq_io_source_core
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${BIOSEQ_IO_SOURCE_CORE_MODULE}")

# DOC0372 §413-424 / DOC0371 §37.2 — the admission controller remains a
# single-owner leaf, while OS probes and MemoryBudget now have one shared
# implementation in bioseq_memory_budget. The compatibility sysinfo facade
# remains in the broad Domain-N module set and only forwards to that library.
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
target_link_libraries(bioseq_memory_admission_core PUBLIC bioseq_memory_budget)
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
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/simd.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/vector_compare/vector_compare.cpp
  ${CMAKE_CURRENT_LIST_DIR}/instrumentation/detail/summary_writer.cpp
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/minimizer_pair_sort_sidecar.cpp)
list(APPEND BIOSEQ_NOPG_SOURCES
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS})
set_source_files_properties(
  ${BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
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
if(BIOSEQ_PACKAGE_BUILD)
  target_compile_definitions(bioseq_modules_nopg PRIVATE
    BIOSEQ_PACKAGE_O0_PROFILE=1
    BIOSEQ_PIPELINE_FORCE_INLINE=0)
endif()
target_compile_definitions(bioseq_modules_nopg PUBLIC
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
target_compile_options(bioseq_modules_nopg PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")

target_include_directories(bioseq_modules_nopg PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../config"
  "${CMAKE_CURRENT_LIST_DIR}/include2"
  "${CMAKE_CURRENT_LIST_DIR}/backend"
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/sorting/hwy_contrib"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/safestringlib/include"
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

# P0 of DOC0398 proves the complete annotation -> prepared Clang closure ->
# fixed-target module -> one generated binder/registry path with SneakySnake.
# Further families extend these explicit registries; target discovery itself
# remains Highway-owned through HWY_VISIT_TARGETS.
set(BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/highway")

# Probe the unattached scheduler source as raw tokens before constructing any
# typed Highway alignment or minimizer slots. The topology manifest is the
# single alphabet/width/shard authority shared by both generators; neither
# frontend may rediscover or multiply the other's domain.
set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_typed_template.cppm")
set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/template_flatten/dispatch_processor_typed")
cmake_path(IS_PREFIX BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}" NORMALIZE
  _bioseq_template_flatten_root_inside_highway_root)
cmake_path(IS_PREFIX BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${BIOSEQ_HIGHWAY_GENERATED_ROOT}" NORMALIZE
  _bioseq_highway_root_inside_template_flatten_root)
if(_bioseq_template_flatten_root_inside_highway_root
    OR _bioseq_highway_root_inside_template_flatten_root)
  message(FATAL_ERROR
    "DOC0398 Highway and template-flatten generated roots must be disjoint; "
    "highway='${BIOSEQ_HIGHWAY_GENERATED_ROOT}', "
    "template_flatten='${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}'")
endif()
unset(_bioseq_highway_root_inside_template_flatten_root)
unset(_bioseq_template_flatten_root_inside_highway_root)
bioseq_template_flatten_probe(
  NAME dispatch_processor_typed
  SOURCE "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.scheduler.dispatch_processor
  PROFILE "${BIOSEQ_OPTTIMIZATION_LEVEL}"
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
    "ksw2|${_bioseq_highway_alignment_family}|ksw2_z2|&::bioseq::simd::{target_namespace}::ksw2_z2<${_bioseq_highway_alignment_alphabet_replacement}>"
    "ksw2|${_bioseq_highway_alignment_family}|ksw2_d2|&::bioseq::simd::{target_namespace}::ksw2_d2<${_bioseq_highway_alignment_alphabet_replacement}>"
    "ksw2_batched|${_bioseq_highway_alignment_family}|ksw2_batched|&::bioseq::simd::{target_namespace}::ksw2_batched<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|block_prepare|&::bioseq::simd::{target_namespace}::block_prepare<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|block_try_certified_gap|&::bioseq::simd::{target_namespace}::block_try_certified_gap<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|block_align|&::bioseq::simd::{target_namespace}::block_align<${_bioseq_highway_alignment_alphabet_replacement}>")
  list(APPEND BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS
    "${_bioseq_highway_alignment_family}|ksw2_z2"
    "${_bioseq_highway_alignment_family}|ksw2_d2"
    "${_bioseq_highway_alignment_family}|ksw2_batched"
    "${_bioseq_highway_alignment_family}|block_prepare"
    "${_bioseq_highway_alignment_family}|block_try_certified_gap"
    "${_bioseq_highway_alignment_family}|block_align")
endforeach()
unset(_bioseq_highway_alignment_family)
unset(_bioseq_highway_alignment_alphabet_label)
unset(_bioseq_highway_alignment_alphabet_replacement)
unset(_bioseq_highway_alignment_alphabet_ordinal)

# Minimizer target wrappers use the same scheduler-owned alphabet/width axes.
# Each synthetic family is one Cartesian case; the Highway renderer validates
# exact 2 x 4 closure and folds these leaves back into the neutral three-member
# MinimizerKernels table. The renderer also reconciles these records with the
# typed alignment families to emit the root table's neutral alphabet-identity
# evidence; no additional CMake alphabet registry participates.
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
      "minimizer_reference|${_bioseq_highway_minimizer_family}|reference_minimap_int|&::bioseq::simd::{target_namespace}::reference_minimap_int_case<${_bioseq_highway_minimizer_template_arguments}>"
      "minimizer_two_stack|${_bioseq_highway_minimizer_family}|two_stack_nthash|&::bioseq::simd::{target_namespace}::two_stack_nthash_case<${_bioseq_highway_minimizer_template_arguments}>"
      "minimizer_paper|${_bioseq_highway_minimizer_family}|paper_nthash|&::bioseq::simd::{target_namespace}::paper_nthash_case<${_bioseq_highway_minimizer_template_arguments}>")
    list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS
      "${_bioseq_highway_minimizer_family}|reference_minimap_int"
      "${_bioseq_highway_minimizer_family}|two_stack_nthash"
      "${_bioseq_highway_minimizer_family}|paper_nthash")
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

bioseq_add_highway_codegen(
  REQUIRE_COMPLETE_TABLE
  TARGET bioseq_highway_target_modules
  POLICY_TARGET bioseq_highway_target_policy
  OUTPUT_ROOT "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
  CONTRACT_MODULE bioseq.highway.dispatch_contract
  DISPATCH_MODULE bioseq.scheduler.dispatch_processor
  HIGHWAY_VERSION 1.3.0
  # Block Aligner requires at least eight lanes. EMU128 remains the portable
  # generated fallback; scalar is not advertised as a complete target table.
  POLICY_DEFINITIONS
    HWY_COMPILE_ALL_ATTAINABLE=1
    HWY_DISABLED_TARGETS=HWY_SCALAR
  DOMAIN_TOPOLOGY_MANIFESTS
    "typed_scheduler=${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_dispatch_contract.cppm"
  PCM_ROOT "${BIOSEQ_HIGHWAY_GENERATED_ROOT}/pcm"
  SOURCE_RECORDS
    "simd_utils|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_utils.cppm|bioseq.highway.sources.simd_utils|simd_utils|simd_utils|"
    "vector_stream|${CMAKE_CURRENT_LIST_DIR}/backend/common/simd/highway_vector_stream.cppm|bioseq.highway.sources.vector_stream|vector_stream|vector_stream|simd_utils"
    "sequence_ops|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/highway_sequence_ops.cppm|bioseq.highway.sources.sequence_ops|sequence_ops|sequence_ops|simd_utils,vector_stream"
    "hasher_base|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_base_kernel.cppm|bioseq.highway.sources.hasher_base|hasher_base|hasher_base|sequence_ops,simd_utils"
    "hasher_lexic|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_lexic_kernel.cppm|bioseq.highway.sources.hasher_lexic|hasher_lexic|hasher_lexic|hasher_base,simd_utils"
    "hasher_minimap_int|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/hashers/highway_minimap_int_kernel.cppm|bioseq.highway.sources.hasher_minimap_int|hasher_minimap_int|hasher_minimap_int|hasher_base,hasher_lexic,simd_utils"
    "minimizer_reference|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/reference_minimap_int_kernel.cppm|bioseq.highway.sources.minimizer_reference|minimizer_reference|minimizers|sequence_ops,hasher_base,hasher_minimap_int,simd_utils,vector_stream"
    "minimizer_two_stack|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm|bioseq.highway.sources.minimizer_two_stack|minimizer_two_stack|minimizers|sequence_ops,hasher_base,hasher_lexic,hasher_minimap_int,simd_utils,vector_stream"
    "minimizer_paper|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/paper_nthash_kernel.cppm|bioseq.highway.sources.minimizer_paper|minimizer_paper|minimizers|sequence_ops,hasher_base,simd_utils,vector_stream"
    "alignment_utils|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/utils_kernel.cppm|bioseq.highway.sources.alignment_utils|alignment_utils|alignment|"
    "alignment_lanes|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/lane_helpers_kernel.cppm|bioseq.highway.sources.alignment_lanes|alignment_lanes|alignment|alignment_utils"
    "ksw2|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_kernel.cppm|bioseq.highway.sources.ksw2|ksw2|alignment|alignment_utils,alignment_lanes"
    "block_aligner|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm|bioseq.highway.sources.block_aligner|block_aligner|alignment|simd_utils,alignment_utils,alignment_lanes"
    "ksw2_batched|${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm|bioseq.highway.sources.ksw2_batched|ksw2_batched|alignment|ksw2,alignment_utils,alignment_lanes"
    "prefilter|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/prefilters/sneaky_snake_kernel.cppm|bioseq.highway.sources.sneaky_snake|prefilter|common|simd_utils"
    "chaining|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/chaining/chaining_simd_kernel.cppm|bioseq.highway.sources.chaining|chaining|chaining|simd_utils"
    "indexing|${CMAKE_CURRENT_LIST_DIR}/backend/sequences/indexing/highway_indexing_kernels.cppm|bioseq.highway.sources.indexing|indexing|indexing|simd_utils"
  SLOT_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS}
    ${BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS}
    "prefilter|common|sneaky_snake_simd_batch|&bioseq::sequences::prefilters::{target_namespace}::sneaky_snake_simd_batch"
    "chaining|chaining|chain_anchors_simd|&::bioseq::simd::{target_namespace}::chain_anchors_simd"
    "indexing|indexing|validate_non_scatter_local32_w2|&bioseq::indexing::{target_namespace}::validate_non_scatter_local32_w2"
  REQUIRED_SLOT_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS}
    ${BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS}
    "common|sneaky_snake_simd_batch"
    "chaining|chain_anchors_simd"
    "indexing|validate_non_scatter_local32_w2"
  OUTPUT_MODULES BIOSEQ_HIGHWAY_GENERATED_MODULES
  OUTPUT_PRIMARY_MODULES BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES
  OUTPUT_TARGET_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS
  OUTPUT_DISPATCH_REGISTRY BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY
  OUTPUT_DISPATCH_INVENTORY BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY
  OUTPUT_TEST_PRIMARY_MODULE BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE
  OUTPUT_TEST_IMPLEMENTATIONS BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS
  OUTPUT_TEST_SUPPORT BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT
  OUTPUT_SOURCE_INPUTS BIOSEQ_HIGHWAY_REGISTERED_SOURCE_INPUTS
  OUTPUT_TARGET_RECORDS BIOSEQ_HIGHWAY_COMPILED_TARGET_RECORDS
  OUTPUT_COMPILED_MASK BIOSEQ_HIGHWAY_COMPILED_TARGET_MASK)

# CMake requires every named module partition provider in a CXX_MODULES file
# set. Keep the primary-module implementation units (binders) as ordinary
# private sources, and attach generated partitions alongside their primaries.
set(BIOSEQ_HIGHWAY_GENERATED_TARGET_BINDERS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TARGET_BINDERS
  INCLUDE REGEX "/binder\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS
  EXCLUDE REGEX "/binder\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TEST_BINDERS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TEST_BINDERS
  INCLUDE REGEX "/binder\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS
  EXCLUDE REGEX "/binder\\.cppm$")

if(NOT BIOSEQ_HIGHWAY_REGISTERED_SOURCE_INPUTS STREQUAL
    BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES)
  message(FATAL_ERROR
    "DOC0398 Highway source registry and final-provider exclusion diverged")
endif()

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

# The SQL compile-contract provider owns the smallest reusable PostgreSQL
# module closure needed by generated catalog handles, row codecs, and GUC
# facets. Both the extension and the dedicated contract OBJECT target import
# these PUBLIC BMIs; the contract objects themselves never enter the DSO.
set(BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/alignment_sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/column/metadata/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/mod_sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/bitmapset.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/datums.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/error_bridge.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/lists.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/runtime_test_counters.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/safety_runtime_probe.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_catalog_entities.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_boundary/aggregate.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_boundary/guc_storage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_boundary/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_boundary/row_codec.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_boundary/vector.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/indexing/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/io/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/sequences_pg/sample_table_udf_sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/table/sql_rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/vectorization/vector_types.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS})

# Compiler-owned DOC0388 module-reachability fixtures. They are removed from
# the production source globs here and compiled only by the dedicated OBJECT
# target declared after generated provider paths are known.
set(BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/catalog_entity_handle_producer_test.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/generated_guc_producer_test.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/sql_boundary_template_producer_test.cppm")
set(BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/catalog_entity_handle_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/generated_guc_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/sql_boundary_template_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/compile_contracts/active_generated_guc_consumer_test.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/sql_entity_handles_contract.cpp")
list(REMOVE_ITEM SRC_MOD_FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})
list(REMOVE_ITEM SRC_FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})
list(REMOVE_DUPLICATES SRC_MOD_FILES)
list(REMOVE_DUPLICATES SRC_FILES)

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
  "Optimization level for GCC 16.1 AWS typed dispatch module units. \
Allowed: O0, O1, O2, O3. O3 preserves the global preset behavior.")
set_property(CACHE BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL PROPERTY STRINGS O0 O1 O2 O3)
if(NOT BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL MATCHES "^O[0123]$")
  message(FATAL_ERROR
    "BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL must be one of O0, O1, O2, O3; got "
    "'${BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL}'")
endif()

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
bioseq_add_template_flatten_codegen(
  NAME dispatch_processor_typed
  SOURCE "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.scheduler.dispatch_processor
  CONFIGURE_TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  EXPECTED_TOPOLOGY_DIGEST "${BIOSEQ_TYPED_TOPOLOGY_DIGEST}"
  ENABLED_ORDINALS ${BIOSEQ_TYPED_ENABLED_ORDINALS}
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor.cppm"
  PCM_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}/pcm"
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
if(BIOSEQ_OPTTIMIZATION_LEVEL STREQUAL "0")
  set(_bioseq_typed_expected_count 7)
elseif(BIOSEQ_OPTTIMIZATION_LEVEL STREQUAL "1")
  set(_bioseq_typed_expected_count 14)
else()
  set(_bioseq_typed_expected_count 56)
endif()
list(LENGTH BIOSEQ_TYPED_IMPLEMENTATION_UNITS
  BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT)
if(NOT BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT EQUAL
    _bioseq_typed_expected_count)
  message(FATAL_ERROR
    "Template-flatten profile ${BIOSEQ_OPTTIMIZATION_LEVEL} requires "
    "${_bioseq_typed_expected_count} scheduler units, generated "
    "${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT}")
endif()
unset(_bioseq_typed_expected_count)

set(BIOSEQ_DISPATCH_PROCESSOR_AUTHORED_IMPLEMENTATION_UNITS
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/mod_impl.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_cpu_preparation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_impl.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_buffer_api_impl.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor_pg_impl.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  ${BIOSEQ_TEMPLATE_FLATTEN_INPUT_MODULES}
  ${BIOSEQ_DISPATCH_PROCESSOR_AUTHORED_IMPLEMENTATION_UNITS})
list(APPEND SRC_FILES
  ${BIOSEQ_DISPATCH_PROCESSOR_AUTHORED_IMPLEMENTATION_UNITS}
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS})
set_source_files_properties(
  ${BIOSEQ_DISPATCH_PROCESSOR_AUTHORED_IMPLEMENTATION_UNITS}
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
set_source_files_properties(
  "${BIOSEQ_TYPED_API_PARTITION}"
  "${BIOSEQ_TYPED_SELECTOR_PARTITION}"
  ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
  PROPERTIES GENERATED TRUE)
message(STATUS
  "bioseq: enabled ${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT} "
  "typed dispatch processor module implementation units")

if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  if(NOT BIOSEQ_GCC_TEMPLATE_FORCE_INLINE)
    set_property(SOURCE ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
      APPEND PROPERTY COMPILE_DEFINITIONS
        BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE=0
        BIOSEQ_PIPELINE_FORCE_INLINE=0)
  endif()
  if(BIOSEQ_GCC_TYPED_DISPATCH_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
      gcc_typed_dispatch_compile=${BIOSEQ_GCC_TYPED_DISPATCH_JOBS})
    set_source_files_properties(${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
      PROPERTIES JOB_POOL_COMPILE gcc_typed_dispatch_compile)
  endif()
  if(NOT BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL STREQUAL "O3")
    set_property(SOURCE ${BIOSEQ_TYPED_IMPLEMENTATION_UNITS}
      APPEND PROPERTY COMPILE_OPTIONS
        "-${BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL}")
  endif()
  message(STATUS
    "bioseq: GCC typed dispatch module units use "
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
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/safestringlib/include"
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

# Unit tests use one generator-proven fixed target, chosen from the compiler's
# HWY_STATIC_TARGET record. The test provider imports the same generated
# canonical partitions as production and never performs runtime dispatch.
if(TARGET bioseq_aligner_tests_exec)
  set_source_files_properties(
    "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}"
    "${BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT}"
    ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS}
    PROPERTIES GENERATED TRUE)
  set_source_files_properties(
    ${BIOSEQ_HIGHWAY_GENERATED_TEST_BINDERS}
    PROPERTIES COMPILE_OPTIONS "-x;c++")
  target_sources(bioseq_aligner_tests_exec
    PRIVATE ${BIOSEQ_HIGHWAY_GENERATED_TEST_BINDERS}
    PRIVATE FILE_SET bioseq_highway_fixed_target_test_modules
      TYPE CXX_MODULES
      BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
      FILES
        "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}"
        "${BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT}"
        ${BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS})
  target_link_libraries(bioseq_aligner_tests_exec PRIVATE
    bioseq_highway_target_policy)
  add_dependencies(
    bioseq_aligner_tests_exec bioseq_highway_target_modules)
endif()

# Test-only backend DSO used by the DOC0388 native alter-function integration
# matrix.  Keep it outside the integration source glob (the .cc suffix) and
# pass its absolute build path only to the external libpq test executable.
if(TARGET bioseq_aligner_int_tests_exec)
  add_library(bioseq_doc0388_alter_hook_probe MODULE EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/integration_tests/doc0388_alter_hook_probe.cc")
  target_link_libraries(bioseq_doc0388_alter_hook_probe PRIVATE
    PostgreSQL::ServerHeaders)
  set_target_properties(bioseq_doc0388_alter_hook_probe PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
    PREFIX "")
  if(APPLE)
    target_link_options(bioseq_doc0388_alter_hook_probe PRIVATE
      "-Wl,-undefined,dynamic_lookup")
  endif()
  add_dependencies(
    bioseq_aligner_int_tests_exec
    bioseq_doc0388_alter_hook_probe)
  target_compile_definitions(bioseq_aligner_int_tests_exec PRIVATE
    "BIOSEQ_DOC0388_ALTER_HOOK_PROBE_PATH=\"$<TARGET_FILE:bioseq_doc0388_alter_hook_probe>\"")
endif()

set(BIOSEQ_SQL_BASE_PHYSICAL_LEDGER
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq-physical-ledger.tsv")
set(BIOSEQ_SQL_EXTERNAL_PHYSICAL_LEDGER_OVERLAY
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq-external-physical-ledger.tsv")
set(BIOSEQ_SQL_RETIRED_EXTERNAL_FRAGMENT
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq_external--1.0.sql")
if(EXISTS "${BIOSEQ_SQL_RETIRED_EXTERNAL_FRAGMENT}")
  message(FATAL_ERROR
    "The optional external SQL surface is generated from natural C++ owners; "
    "the retired bioseq_external--1.0.sql fragment must not be restored")
endif()
set(BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER
  "${BIOSEQ_SQL_BASE_PHYSICAL_LEDGER}")
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  file(READ "${BIOSEQ_SQL_BASE_PHYSICAL_LEDGER}"
    _bioseq_sql_base_physical_ledger)
  if(NOT _bioseq_sql_base_physical_ledger MATCHES "\n$")
    message(FATAL_ERROR
      "Base SQL physical ledger must end with a newline")
  endif()
  file(STRINGS "${BIOSEQ_SQL_EXTERNAL_PHYSICAL_LEDGER_OVERLAY}"
    _bioseq_sql_external_physical_rows
    REGEX "^[^#].+")
  list(LENGTH _bioseq_sql_external_physical_rows
    _bioseq_sql_external_physical_row_count)
  if(NOT _bioseq_sql_external_physical_row_count EQUAL 13)
    message(FATAL_ERROR
      "External SQL physical ledger overlay must contain exactly 13 rows")
  endif()
  list(GET _bioseq_sql_external_physical_rows 0
    _bioseq_sql_external_physical_first)
  list(GET _bioseq_sql_external_physical_rows -1
    _bioseq_sql_external_physical_last)
  if(NOT _bioseq_sql_external_physical_first MATCHES "^P488\t"
      OR NOT _bioseq_sql_external_physical_last MATCHES "^P500\t")
    message(FATAL_ERROR
      "External SQL physical ledger overlay must span P488 through P500")
  endif()
  string(JOIN "\n" _bioseq_sql_external_physical_body
    ${_bioseq_sql_external_physical_rows})
  set(_bioseq_sql_external_physical_ledger_dir
    "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/physical-ledger")
  file(MAKE_DIRECTORY
    "${_bioseq_sql_external_physical_ledger_dir}")
  set(BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER
    "${_bioseq_sql_external_physical_ledger_dir}/bioseq-external-physical-ledger.tsv")
  file(WRITE "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
    "${_bioseq_sql_base_physical_ledger}"
    "${_bioseq_sql_external_physical_body}\n")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql"
  "${BIOSEQ_SQL_BASE_PHYSICAL_LEDGER}"
  "${BIOSEQ_SQL_EXTERNAL_PHYSICAL_LEDGER_OVERLAY}")

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqSqlCodegen.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/PostgresErrorSafety.cmake")
include(
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/RuntimeEvidence.cmake")

target_link_libraries(bioseq_modules_nopg
  PUBLIC bioseq_sql_annotation_model)

add_library(bioseq_sql_annotation_probe OBJECT
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/annotation_probe.cpp")
target_sources(bioseq_sql_annotation_probe
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../codegen"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/annotation_probe.cppm")
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

# The collector receives the configured backend module closure, ignores
# modules without bioseq_sql annotations, and never reads the checked SQL.
# The ordinary target is a stale-golden gate; updating the source-tree golden
# requires the explicitly invoked maintainer target.
option(BIOSEQ_TEST_INDEXES
  "Compile the test-only indexing descriptors into the extension + test execs"
  OFF)
set(BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/backend/pg/pg_utils/row_codec_runtime_test_support.cppm")
list(REMOVE_ITEM SRC_MOD_FILES
  "${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE}")
# Both profile prepares need this named-module provider in the raw compile
# database. The extension imports and executes its test API only when
# BIOSEQ_TEST_INDEXES is enabled.
list(APPEND SRC_MOD_FILES
  "${BIOSEQ_SQL_ROW_CODEC_RUNTIME_TEST_MODULE}")
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
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/validate_evidence.py")
set(BIOSEQ_SQL_SAFETY_PROVENANCE
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/fixtures/pg17_provenance.json")
set(BIOSEQ_SQL_SAFETY_RUNTIME_POLICY
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/fixtures/doc0388_runtime_policy.json")
set(BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE "" CACHE FILEPATH
  "Reviewed profile-neutral DOC0388 PostgreSQL runtime case template")
set(BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR "" CACHE PATH
  "Inverse-profile runtime-instrumented Bioseq build tree")
set(BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE "isolated" CACHE STRING
  "Runtime evidence cluster relationship (isolated only)")
set_property(CACHE BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE
  PROPERTY STRINGS isolated)
set(BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS "600" CACHE STRING
  "Per-child timeout for PostgreSQL runtime evidence")
mark_as_advanced(
  BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE
  BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR
  BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE
  BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS)
set(BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/validator-self-test.stamp")
set(BIOSEQ_SQL_PREPARE_INCREMENTAL_FIXTURE
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/clang_structural/test_prepare_incremental.py")
set(BIOSEQ_SQL_PREPARE_INCREMENTAL_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/prepare-incremental-self-test.stamp")
if(NOT EXISTS "${BIOSEQ_SQL_VALIDATOR}")
  message(FATAL_ERROR
    "Missing PostgreSQL safety evidence validator: ${BIOSEQ_SQL_VALIDATOR}")
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
  COMMENT "Running PostgreSQL safety evidence validator self-tests"
  VERBATIM)
add_custom_target(bioseq_sql_validator_self_test
  DEPENDS "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}")
set(BIOSEQ_SQL_NEGATIVE_FIXTURES
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/invalid_nested_annotation_namespace.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/invalid_class_head_placement.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/invalid_guc_enumerator_placement.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/invalid_mixed_guc_alias_initialization.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/authored_catalog_handle_specialization.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/authored_guc_registration_access.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/authored_catalog_handle_namespace_alias.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/authored_migration_install_owner.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/authored_guc_access_using_declaration.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/negative/authored_guc_registration_type_alias.cppm")
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
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/contract_tests.cppm"
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
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_MANAGER
    "${_bioseq_sql_fmgr_entrypoints_dir}/storage_manager_sequence_storage_manager.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS
    "${_bioseq_sql_fmgr_entrypoints_dir}/table_access_hooks.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG
    "${_bioseq_sql_fmgr_entrypoints_dir}/storage_load_pg.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF
    "${_bioseq_sql_fmgr_entrypoints_dir}/sequences_sample_table_udf.entrypoints.cppm")
  set(BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE
    "${_bioseq_sql_fmgr_entrypoints_dir}/types_pg_compact_sequence.entrypoints.cppm")
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
endforeach()
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_SQL_ROW_CODEC_MODULES
    ${BIOSEQ_SQL_TEST_INDEXES_ROW_CODEC_MODULES})
  set(BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES
    ${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES})
  set(_bioseq_sql_active_profile_key TEST_INDEXES)
else()
  set(BIOSEQ_SQL_ROW_CODEC_MODULES
    ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES})
  set(BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES
    ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES})
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
  "Reviewed production/test-index PostgreSQL safety evidence bundle")
mark_as_advanced(BIOSEQ_SQL_SAFETY_EVIDENCE_DIR)
set(_bioseq_sql_safety_evidence_inputs)
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
  set(_bioseq_sql_safety_evidence_inputs
    "${BIOSEQ_SQL_SAFETY_PRODUCTION_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_PRODUCTION_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_TEST_INDEXES_LEDGER}"
    "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_MATRIX}"
    "${BIOSEQ_SQL_SAFETY_REVIEWED_RUNTIME_MATRIX}")
  foreach(_bioseq_sql_safety_input
      IN LISTS _bioseq_sql_safety_evidence_inputs)
    if(NOT EXISTS "${_bioseq_sql_safety_input}")
      message(FATAL_ERROR
        "Incomplete PostgreSQL safety evidence bundle: missing "
        "${_bioseq_sql_safety_input}")
    endif()
  endforeach()
endif()
set(BIOSEQ_SQL_GUC_PARITY_LEDGER
  "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_guc_parity_ledger.json")
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
  MODULE bioseq.aligner.sql_guc_settings
  SOURCES backend/config/aligner_sql_guc_settings.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS aligner)
bioseq_register_sql_owner(
  OWNER_KEY scheduler_guc
  MODULE bioseq.scheduler.sql_guc_settings
  SOURCES backend/config/scheduler_sql_guc_settings.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS scheduler)
bioseq_register_sql_owner(
  OWNER_KEY extension_guc
  MODULE bioseq.sql_guc_settings
  SOURCES backend/config/sql_guc_settings.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS bioseq)
bioseq_register_sql_owner(
  OWNER_KEY storage_guc
  MODULE bioseq.storage.sql_guc_settings
  SOURCES backend/config/storage_sql_guc_settings.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  GUC_OWNERS storage)

bioseq_register_sql_owner(
  OWNER_KEY alignment_rows
  MODULE bioseq.mod.alignment.sql_rows
  SOURCES backend/pg/alignment_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY storage_metadata_rows
  MODULE bioseq.storage.metadata.sql_rows
  SOURCES backend/pg/column/metadata/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_metadata)
bioseq_register_sql_owner(
  OWNER_KEY mod_rows
  MODULE bioseq.mod.sql_rows
  SOURCES backend/pg/mod_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY sequences_sample_table_udf_rows
  MODULE bioseq.sequences.sample_table_udf.sql_rows
  SOURCES backend/pg/sequences_pg/sample_table_udf_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ANNOTATION_FREE_RESULT_ROWS sequences_sample_table_udf)
bioseq_register_sql_owner(
  OWNER_KEY sql_function_rows
  MODULE bioseq.sql.functions.sql_rows
  SOURCES backend/pg/sql/functions_sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  OWNER_KEY storage_indexing_rows
  MODULE bioseq.storage.indexing_pg.sql_rows
  SOURCES backend/pg/storage_pg/indexing/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_indexing_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_load_rows
  MODULE bioseq.storage.load_pg.sql_rows
  SOURCES backend/pg/storage_pg/io/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_load_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_rows
  MODULE bioseq.storage.manager.sql_rows
  SOURCES backend/pg/storage_pg/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY table_rows
  MODULE bioseq.table.sql_rows
  SOURCES backend/pg/table/sql_rows.cppm
  PROFILES production test_indexes
  AUTHORITIES SUPPORT
  ROW_CODECS table)

bioseq_register_sql_owner(
  OWNER_KEY storage_metadata_core
  MODULE bioseq.storage_metadata.core
  SOURCES backend/pg/column/metadata/core_sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_metadata_core)
bioseq_register_sql_owner(
  OWNER_KEY mod
  MODULE bioseq.mod
  SOURCES backend/pg/mod.cppm
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
  OWNER_KEY sequences_sample_table_udf
  MODULE bioseq.sequences.sample_table_udf
  SOURCES backend/pg/sequences_pg/sample_table_udf.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS sequences_sample_table_udf)
bioseq_register_sql_owner(
  OWNER_KEY sql_bootstrap
  MODULE bioseq.sql.bootstrap
  SOURCES backend/pg/sql/bootstrap.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY sql_catalog_views
  MODULE bioseq.sql.catalog_views
  SOURCES backend/pg/sql/catalog_views.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY sql_functions
  MODULE bioseq.sql.functions
  SOURCES backend/pg/sql/functions.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY external_relation_legacy_fmgr
  MODULE bioseq.pg.external.relation
  SOURCES backend/pg/external/relation.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  LEGACY_FMGR_SCANS bioseq_external_handler)
bioseq_register_sql_owner(
  OWNER_KEY external_sql_catalog
  MODULE bioseq.pg.external.sql_catalog
  SOURCES backend/pg/external/sql_catalog.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  OWNER_KEY storage_indexing_pg_chunked_storage
  MODULE bioseq.storage_indexing_pg.chunked_storage
  SOURCES backend/pg/storage_pg/indexing/chunked_storage_sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_indexing_pg_chunked_storage)
bioseq_register_sql_owner(
  OWNER_KEY storage_load_pg
  MODULE bioseq.storage.load_pg
  SOURCES backend/pg/storage_pg/io/load.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS storage_load_pg)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_sequence_storage_manager_core
  MODULE bioseq.storage_manager.sequence_storage_manager
  SOURCES backend/pg/storage_pg/sequence_storage_manager_sql_entities.cppm
  PROFILES production test_indexes
  AUTHORITIES CORE
  ENTITY_HANDLES storage_manager_sequence_storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY storage_manager_sequence_storage_manager
  MODULE bioseq.storage.manager:sequence_storage_manager
  SOURCES backend/pg/storage_pg/sequence_storage_manager.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS storage_manager_sequence_storage_manager)
bioseq_register_sql_owner(
  OWNER_KEY table
  MODULE bioseq.table
  SOURCES
    backend/pg/table/access_hooks.cppm
    backend/pg/table/access_method.cppm
    backend/pg/table/custom_scan.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS table_access_hooks)
bioseq_register_sql_owner(
  OWNER_KEY types_pg_compact_sequence
  MODULE bioseq.types.pg_compact_sequence
  SOURCES
    backend/pg/types/compact_sequence/array_string.cppm
    backend/pg/types/compact_sequence/concat_reverse.cppm
    backend/pg/types/compact_sequence/content.cppm
    backend/pg/types/compact_sequence/flat_abi.cppm
    backend/pg/types/compact_sequence/io.cppm
    backend/pg/types/compact_sequence/letter.cppm
    backend/pg/types/compact_sequence/packed_output.cppm
    backend/pg/types/compact_sequence/sql_catalog.cppm
    backend/pg/types/compact_sequence/subscript.cppm
    backend/pg/types/compact_sequence/tree_ops.cppm
  PROFILES production test_indexes
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS types_pg_compact_sequence)
bioseq_register_sql_owner(
  OWNER_KEY vector_types
  MODULE bioseq.vector.types
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
    vector_aggregate_registry)

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
    storage_manager_sequence_storage_manager
    table_access_hooks
    storage_load_pg
    sequences_sample_table_udf
    types_pg_compact_sequence
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
  EXPECTED_LEGACY_FMGR_SCANS
    bioseq_external_handler)

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
      "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
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
      --fmgr-entrypoints-output
        "vector_comparison_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_COMPARISON_REGISTRY}"
      --fmgr-entrypoints-output
        "vector_aggregate_registry=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_VECTOR_AGGREGATE_REGISTRY}"
      --guc-parity-ledger "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
      --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
      --guc-active-profile "${_bioseq_sql_profile}"
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
      "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
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
    "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
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
    # FINAL preparation precompiles the full bioseq.mod closure. Keep the
    # production and test_indexes variants out of memory-pressure contention.
    JOB_POOL sql_codegen_ast_proof
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
  set(_bioseq_sql_golden_args)
  if(_bioseq_sql_profile STREQUAL "production"
      AND NOT BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
    set(_bioseq_sql_golden_args
      --golden "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql")
  endif()

  add_custom_command(
    OUTPUT "${_bioseq_sql_generated_stamp}"
    BYPRODUCTS
      "${_bioseq_sql_generated_install}"
      "${_bioseq_sql_generated_manifest}"
      ${_bioseq_sql_fmgr_modules}
    COMMAND "${CMAKE_COMMAND}" -E env
      "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
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
      --physical-ledger "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
      --require-checked-physical-ledger
      ${_bioseq_sql_golden_args}
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
        "types_pg_compact_sequence=${BIOSEQ_SQL_${_bioseq_sql_profile_key}_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
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
      --guc-parity-ledger "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
      --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
      --guc-active-profile "${_bioseq_sql_profile}"
      ${_bioseq_sql_completion_args}
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
      "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql"
      "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
      "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
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

# The raw compile database names the generated core entity-handle providers
# before their source files exist on a fresh tree.  Template-flatten PREPARE
# reaches those providers while recursively scanning the scheduler primary, so
# order its dependency-only scan after the profile-neutral CORE stage.  Do not
# wait for either FINAL profile: those much wider closures are not scheduler
# inputs.  Append the concrete stamp as a file dependency because a target-only
# edge would allow Ninja to run the two custom commands concurrently.
add_custom_command(
  OUTPUT
    "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}/pcm/metadata/prepare_compile_commands.json"
  APPEND
  DEPENDS "${BIOSEQ_SQL_CORE_GENERATED_STAMP}")

# The production determinism gate regenerates the complete SUPPORT/CORE/FINAL
# artifact closure from canonical, reversed, and deterministically shuffled
# owner inventories.  Each process has a distinct working/output root and a
# symlink-free `workspace/main` mirror of selected project source/build inputs.
# Source, context, and output paths preserve their ordinary source-root-relative
# identities. Generated outputs and same-root PREPARE closures are compared as
# exact bytes; independently validated cross-root PREPARE manifests are compared
# through their exact relocation-semantic projections.
set(BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/verify_production_determinism.py")
set(BIOSEQ_SQL_PRODUCTION_DETERMINISM_ROOT
  "${BIOSEQ_SQL_CODEGEN_DIR}/production-determinism")
set(BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT
  "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_ROOT}/report.json")
set(BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP
  "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_ROOT}/self-test.stamp")
set(BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY
  "${BIOSEQ_SQL_CODEGEN_DIR}/required-target-dyndeps.tsv")
if(NOT EXISTS "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT}")
  message(FATAL_ERROR
    "Missing production SQL determinism gate: "
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT}")
endif()

# PREPARE consumes every C++ module row in CMake's raw compile database.
# Depending only on the extension dyndep therefore leaves unrelated, unique
# response-file classes (bench, cache, and compiler-contract fixtures)
# unmaterialized in a clean tree.  These top-level dyndep anchors are the
# bounded minimum that covers every current source+standard equivalence class
# through CMake's generated CXXModules dependencies.  The determinism harness
# still rejects any newly introduced unique class not covered by these anchors.
#
# Resolve paths from target BINARY_DIR only after this directory has declared
# its benchmark targets.  File dependencies materialize scans/modmaps without
# making the compile targets depend on SQL proofs; the extension proof remains
# a link-only dependency below.
function(_bioseq_finalize_sql_target_dyndep_anchors)
  set(_bioseq_sql_target_dyndep_anchors
    bioseq_bench
    bioseq_bench_e2e_tests_exec
    bioseq_sql_annotation_probe
    bioseq_sql_gcc_compile_contract_objects
    postgresql_bioseq)
  if(BIOSEQ_BUILD_CACHE)
    list(APPEND _bioseq_sql_target_dyndep_anchors
      postgresql_bioseq_cache)
  endif()
  list(SORT _bioseq_sql_target_dyndep_anchors)

  string(CONCAT _bioseq_sql_target_dyndep_inventory
    "schema\tbioseq_sql_target_dyndep_inventory/v1\n"
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
      "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"
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

add_custom_command(
  OUTPUT "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
    "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT}" self-test
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP}"
  DEPENDS "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT}"
  COMMENT
    "Running production SQL determinism mutation self-tests"
  VERBATIM)
add_custom_target(bioseq_sql_production_determinism_self_test
  DEPENDS "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP}")

add_custom_command(
  OUTPUT "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"
  COMMAND "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
    "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT}" run
    --codegen "$<TARGET_FILE:bioseq_codegen>"
    --owner-registry-metadata
      "${BIOSEQ_SQL_OWNER_REGISTRY_METADATA}"
    --support-source-list
      "${BIOSEQ_SQL_REGISTRY_PRODUCTION_SUPPORT_FILE}"
    --test-indexes-support-source-list
      "${BIOSEQ_SQL_REGISTRY_TEST_INDEXES_SUPPORT_FILE}"
    --core-source-list
      "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    --final-source-list
      "${BIOSEQ_SQL_REGISTRY_PRODUCTION_FINAL_FILE}"
    --raw-compile-database
      "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    --required-target-dyndep-inventory
      "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"
    --source-root
      "${CMAKE_CURRENT_LIST_DIR}/.."
    --build-root
      "${CMAKE_BINARY_DIR}"
    --ordinary-context-root
      "${BIOSEQ_SQL_CLANG_CONTEXT_ROOT}"
    --ordinary-output
      "artifacts/support/support-manifest.json=${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
    --ordinary-output
      "artifacts/support/support.stamp=${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
    --ordinary-output
      "artifacts/test-support/support-manifest.json=${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
    --ordinary-output
      "artifacts/test-support/support.stamp=${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
    --ordinary-output
      "artifacts/core/core-manifest.json=${BIOSEQ_SQL_CORE_MANIFEST}"
    --ordinary-output
      "artifacts/core/core-descriptor.sha256=${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    --ordinary-output
      "artifacts/core/core.stamp=${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    --ordinary-output
      "artifacts/final/bioseq--1.0.sql=${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
    --ordinary-output
      "artifacts/final/bioseq-sql-manifest.json=${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    --ordinary-output
      "artifacts/final/bioseq-sql.stamp=${BIOSEQ_SQL_PRODUCTION_GENERATED_STAMP}"
    --ordinary-output
      "artifacts/row-codecs/storage_metadata.row_codecs.cppm=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_METADATA}"
    --ordinary-output
      "artifacts/row-codecs/storage_indexing_pg.row_codecs.cppm=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_INDEXING_PG}"
    --ordinary-output
      "artifacts/row-codecs/storage_load_pg.row_codecs.cppm=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_LOAD_PG}"
    --ordinary-output
      "artifacts/row-codecs/storage_manager.row_codecs.cppm=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_MANAGER}"
    --ordinary-output
      "artifacts/row-codecs/table.row_codecs.cppm=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_TABLE}"
    --ordinary-output
      "artifacts/test-row-codecs/storage_metadata.row_codecs.cppm=${BIOSEQ_SQL_TEST_INDEXES_ROW_CODECS_STORAGE_METADATA}"
    --ordinary-output
      "artifacts/test-row-codecs/storage_indexing_pg.row_codecs.cppm=${BIOSEQ_SQL_TEST_INDEXES_ROW_CODECS_STORAGE_INDEXING_PG}"
    --ordinary-output
      "artifacts/test-row-codecs/storage_load_pg.row_codecs.cppm=${BIOSEQ_SQL_TEST_INDEXES_ROW_CODECS_STORAGE_LOAD_PG}"
    --ordinary-output
      "artifacts/test-row-codecs/storage_manager.row_codecs.cppm=${BIOSEQ_SQL_TEST_INDEXES_ROW_CODECS_STORAGE_MANAGER}"
    --ordinary-output
      "artifacts/test-row-codecs/table.row_codecs.cppm=${BIOSEQ_SQL_TEST_INDEXES_ROW_CODECS_TABLE}"
    --ordinary-output
      "artifacts/handles/storage_metadata_core.entity_handles.cppm=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_METADATA_CORE}"
    --ordinary-output
      "artifacts/handles/storage_indexing_pg_chunked_storage.entity_handles.cppm=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_INDEXING_CHUNKED}"
    --ordinary-output
      "artifacts/handles/storage_manager_sequence_storage_manager.entity_handles.cppm=${BIOSEQ_SQL_ENTITY_HANDLES_STORAGE_MANAGER_SEQUENCE}"
    --ordinary-output
      "artifacts/fmgr/mod.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_MOD}"
    --ordinary-output
      "artifacts/fmgr/highway_target_test_support.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_HIGHWAY_TARGET_TEST_SUPPORT}"
    --ordinary-output
      "artifacts/fmgr/storage_manager_sequence_storage_manager.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_STORAGE_MANAGER}"
    --ordinary-output
      "artifacts/fmgr/table_access_hooks.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_TABLE_ACCESS_HOOKS}"
    --ordinary-output
      "artifacts/fmgr/storage_load_pg.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_STORAGE_LOAD_PG}"
    --ordinary-output
      "artifacts/fmgr/sequences_sample_table_udf.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_SEQUENCES_SAMPLE_TABLE_UDF}"
    --ordinary-output
      "artifacts/fmgr/types_pg_compact_sequence.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_COMPACT_SEQUENCE}"
    --ordinary-output
      "artifacts/fmgr/vector_types_integer.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_VECTOR_TYPES_INTEGER}"
    --ordinary-output
      "artifacts/fmgr/vector_types_date.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_VECTOR_TYPES_DATE}"
    --ordinary-output
      "artifacts/fmgr/vector_types_aggregates.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINTS_VECTOR_TYPES_AGGREGATES}"
    --ordinary-output
      "artifacts/fmgr/vector_comparison_registry.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_VECTOR_COMPARISON_REGISTRY}"
    --ordinary-output
      "artifacts/fmgr/vector_aggregate_registry.entrypoints.cppm=${BIOSEQ_SQL_PRODUCTION_VECTOR_AGGREGATE_REGISTRY}"
    --ordinary-output
      "artifacts/test-fmgr/vector_comparison_registry.entrypoints.cppm=${BIOSEQ_SQL_TEST_INDEXES_VECTOR_COMPARISON_REGISTRY}"
    --ordinary-output
      "artifacts/test-fmgr/vector_aggregate_registry.entrypoints.cppm=${BIOSEQ_SQL_TEST_INDEXES_VECTOR_AGGREGATE_REGISTRY}"
    --ordinary-output
      "artifacts/gucs/production/guc_registry.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/production/guc_registry.cppm"
    --ordinary-output
      "artifacts/gucs/production/owners/bioseq.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/production/owners/bioseq.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/production/owners/scheduler.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/production/owners/scheduler.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/production/owners/storage.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/production/owners/storage.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/production/owners/aligner.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/production/owners/aligner.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/test_indexes/guc_registry.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/test_indexes/guc_registry.cppm"
    --ordinary-output
      "artifacts/gucs/test_indexes/owners/bioseq.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/test_indexes/owners/bioseq.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/test_indexes/owners/scheduler.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/test_indexes/owners/scheduler.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/test_indexes/owners/storage.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/test_indexes/owners/storage.gucs.cppm"
    --ordinary-output
      "artifacts/gucs/test_indexes/owners/aligner.gucs.cppm=${BIOSEQ_SQL_GUC_OUTPUT_ROOT}/test_indexes/owners/aligner.gucs.cppm"
    --physical-ledger
      "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
    --guc-parity-ledger
      "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
    --clang-binary
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    --clang-scan-deps
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    --llvm-config
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    --compiler-module-inventory
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    --clang-sha256
      "${BIOSEQ_CODEGEN_CLANG_SHA256}"
    --clang-scan-deps-sha256
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
    --llvm-config-sha256
      "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
    --compiler-module-inventory-sha256
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
    --output-root
      "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_ROOT}"
    --report
      "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT}"
    "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP}"
    "${BIOSEQ_SQL_OWNER_REGISTRY_METADATA}"
    "${BIOSEQ_SQL_RAW_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_REGISTRY_PRODUCTION_SUPPORT_FILE}"
    ${BIOSEQ_SQL_REGISTRY_PRODUCTION_SUPPORT_SOURCES}
    "${BIOSEQ_SQL_REGISTRY_TEST_INDEXES_SUPPORT_FILE}"
    ${BIOSEQ_SQL_REGISTRY_TEST_INDEXES_SUPPORT_SOURCES}
    "${BIOSEQ_SQL_REGISTRY_CORE_FILE}"
    ${BIOSEQ_SQL_REGISTRY_CORE_SOURCES}
    "${BIOSEQ_SQL_REGISTRY_PRODUCTION_FINAL_FILE}"
    ${BIOSEQ_SQL_REGISTRY_PRODUCTION_FINAL_SOURCES}
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
    "${BIOSEQ_SQL_CORE_MANIFEST}"
    "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    "${BIOSEQ_SQL_PRODUCTION_GENERATED_STAMP}"
    ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES}
    ${BIOSEQ_SQL_TEST_INDEXES_ROW_CODEC_MODULES}
    ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
    ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES}
    ${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_VECTOR_REGISTRY_MODULES}
    ${BIOSEQ_SQL_GUC_production_MODULES}
    ${BIOSEQ_SQL_GUC_test_indexes_MODULES}
    "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
    "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
    "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
  JOB_POOL sql_codegen_ast_proof
  COMMENT
    "Proving production SQL generation is byte-identical under owner discovery permutations"
  VERBATIM)
add_custom_target(bioseq_sql_production_determinism
  DEPENDS "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}")
# Full canonical/reversed/shuffled replay is an explicit opt-in proof.  Default
# extension linking retains the cheap mutation self-tests and all ordinary
# generation, ledger, and static/GCC gates below.
add_dependencies(
  bioseq_sql_production_determinism
  bioseq_sql_production_determinism_self_test
  bioseq_sql_final_production)

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
  "${CMAKE_CURRENT_LIST_DIR}/../config"
  "${CMAKE_CURRENT_LIST_DIR}/include2"
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
add_dependencies(
  bioseq_sql_compile_contract_providers
  bioseq_sql_final)

add_library(bioseq_sql_gcc_compile_contract_objects OBJECT EXCLUDE_FROM_ALL
  ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})
target_sources(bioseq_sql_gcc_compile_contract_objects
  PRIVATE
    FILE_SET bioseq_sql_gcc_contract_modules
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES ${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})
target_include_directories(bioseq_sql_gcc_compile_contract_objects PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../config"
  "${CMAKE_CURRENT_LIST_DIR}/include2"
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
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/verify_gcc_only_contracts.py")
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
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/PostgresErrorSafety.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqCodegen.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqGccToolchain.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/gcc_hwy_header_unit_launcher.py"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/options.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/contract_tests.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/annotation_probe.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/annotation_probe.cppm"
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
    --repository-root "${CMAKE_CURRENT_LIST_DIR}/../.."
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
set(BIOSEQ_SQL_SAFETY_GENERATOR_REVISION
  "bioseq-postgres-error-safety-v2")
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
    GENERATOR_REVISION "${BIOSEQ_SQL_SAFETY_GENERATOR_REVISION}"
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
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/verify_external_feature_contract.py")
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
      --base-golden
        "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql"
      --expect "${_bioseq_sql_external_feature_expect}"
      --stamp "${_bioseq_sql_external_feature_contract_stamp}"
    DEPENDS
      "${BIOSEQ_SQL_EXTERNAL_FEATURE_CONTRACT_SCRIPT}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_STAMP}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_INSTALL}"
      "${BIOSEQ_SQL_${_bioseq_sql_profile_key}_GENERATED_MANIFEST}"
      "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql"
    COMMENT
      "Verifying ${_bioseq_sql_profile} external SQL feature contract"
    VERBATIM)
  list(APPEND _bioseq_sql_external_feature_contract_stamps
    "${_bioseq_sql_external_feature_contract_stamp}")
endforeach()
add_custom_target(bioseq_sql_external_feature_contract
  DEPENDS ${_bioseq_sql_external_feature_contract_stamps})
add_dependencies(
  bioseq_sql_external_feature_contract
  bioseq_sql_production_determinism_self_test)

find_package(Git REQUIRED)
if(NOT BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
    AND BIOSEQ_DOC0388_RUN_LEDGER_MAINTAINER_CHECK)
  set(BIOSEQ_DOC0388_LEDGER_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/doc0388_ledgers.py")
  set(BIOSEQ_DOC0388_LEDGER_FIXTURES
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_catalog_delta.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_catalog_join_projection.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_fmgr_migration_inventory.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_ledger_schemas.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_result_identity_ledger.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_row_authority_migration_ledger.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_statement_disposition_ledger.json"
    "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_statement_disposition_rules.json")
  add_custom_target(bioseq_doc0388_ledgers
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${Python3_EXECUTABLE}"
      "${BIOSEQ_DOC0388_LEDGER_SCRIPT}"
      check
      --repository-root "${CMAKE_CURRENT_LIST_DIR}/../.."
      --candidate-manifest
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
      --git "${GIT_EXECUTABLE}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${Python3_EXECUTABLE}"
      "${BIOSEQ_DOC0388_LEDGER_SCRIPT}"
      self-test
      --repository-root "${CMAKE_CURRENT_LIST_DIR}/../.."
      --candidate-manifest
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
      --git "${GIT_EXECUTABLE}"
    DEPENDS
      "${BIOSEQ_DOC0388_LEDGER_SCRIPT}"
      ${BIOSEQ_DOC0388_LEDGER_FIXTURES}
      "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
    COMMENT
      "Checking exact DOC0388 source-to-target migration ledgers"
    VERBATIM)
  add_dependencies(
    bioseq_doc0388_ledgers
    bioseq_sql_final_production)
endif()

set(_bioseq_sql_check_golden_outputs
  ${_bioseq_sql_final_stamps}
  ${_bioseq_sql_external_feature_contract_stamps})
# The full post-FINAL Clang facts/proof closure is intentionally opt-in.  Keep
# its static validator report available without pulling it into the ordinary
# generated-SQL/golden or extension-link gates.
if(NOT "${_bioseq_sql_safety_evidence_inputs}" STREQUAL "")
  set(BIOSEQ_SQL_SAFETY_EVIDENCE_REPORT
    "${BIOSEQ_SQL_CODEGEN_DIR}/bioseq-safety-static-evidence.json")
  add_custom_command(
    OUTPUT "${BIOSEQ_SQL_SAFETY_EVIDENCE_REPORT}"
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
      --report "${BIOSEQ_SQL_SAFETY_EVIDENCE_REPORT}"
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
      ${_bioseq_sql_safety_evidence_inputs}
    COMMENT
      "Validating current PostgreSQL ERROR-safety ledgers against reviewed static evidence"
    VERBATIM)
  add_custom_target(bioseq_sql_postgres_error_safety_static_evidence
    DEPENDS "${BIOSEQ_SQL_SAFETY_EVIDENCE_REPORT}")
endif()
add_custom_target(bioseq_sql_check_golden
  DEPENDS ${_bioseq_sql_check_golden_outputs})
add_dependencies(
  bioseq_sql_check_golden
  bioseq_sql_validator_self_test
  bioseq_sql_external_feature_contract
  bioseq_sql_production_determinism_self_test)
if(TARGET bioseq_doc0388_ledgers)
  add_dependencies(
    bioseq_sql_check_golden
    bioseq_doc0388_ledgers)
endif()

# The checked golden is the feature-disabled P001-P487 base package.  An
# external-enabled configuration may verify its generated P488-P500 overlay,
# but must never publish that optional package into the base source golden.
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
add_custom_target(bioseq_sql_update_golden
  COMMAND "${CMAKE_COMMAND}" -E echo
    "bioseq_sql_update_golden is disabled when BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON; configure the base profile to update the P001-P487 golden"
  COMMAND "${CMAKE_COMMAND}" -E false
  VERBATIM)
else()
# The maintainer command must not share writable final artifacts or the success
# stamp with the ordinary production gate.  It verifies the canonical support
# row/GUC bytes, but writes every FINAL candidate under its own directory.
set(BIOSEQ_SQL_UPDATE_GOLDEN_DIR
  "${BIOSEQ_SQL_CODEGEN_DIR}/update-golden")
set(BIOSEQ_SQL_UPDATE_GOLDEN_INSTALL
  "${BIOSEQ_SQL_UPDATE_GOLDEN_DIR}/bioseq--1.0.sql")
set(BIOSEQ_SQL_UPDATE_GOLDEN_MANIFEST
  "${BIOSEQ_SQL_UPDATE_GOLDEN_DIR}/bioseq-sql-manifest.json")
set(BIOSEQ_SQL_UPDATE_GOLDEN_STAMP
  "${BIOSEQ_SQL_UPDATE_GOLDEN_DIR}/bioseq-sql.stamp")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR
  "${BIOSEQ_SQL_UPDATE_GOLDEN_DIR}/fmgr")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_MOD
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/mod.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_HIGHWAY_TARGET_TEST_SUPPORT
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/highway_target_test_support.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_STORAGE_MANAGER
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/storage_manager_sequence_storage_manager.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_TABLE_ACCESS_HOOKS
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/table_access_hooks.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_STORAGE_LOAD_PG
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/storage_load_pg.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_SAMPLE_TABLE_UDF
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/sequences_sample_table_udf.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_COMPACT_SEQUENCE
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/types_pg_compact_sequence.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_INTEGER
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/vector_types_integer.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_DATE
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/vector_types_date.entrypoints.cppm")
set(BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_AGGREGATES
  "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_DIR}/vector_types_aggregates.entrypoints.cppm")
add_custom_target(bioseq_sql_update_golden
  BYPRODUCTS
    "${BIOSEQ_SQL_UPDATE_GOLDEN_INSTALL}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_MANIFEST}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_STAMP}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_MOD}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_HIGHWAY_TARGET_TEST_SUPPORT}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_STORAGE_MANAGER}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_TABLE_ACCESS_HOOKS}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_STORAGE_LOAD_PG}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_SAMPLE_TABLE_UDF}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_COMPACT_SEQUENCE}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_INTEGER}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_DATE}"
    "${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_AGGREGATES}"
  COMMAND
    "$<TARGET_FILE:bioseq_codegen>" sql update-golden
    --stage final
    --profile production
    --source-list "${BIOSEQ_SQL_REGISTRY_PRODUCTION_FINAL_FILE}"
    --compile-database "${BIOSEQ_SQL_PRODUCTION_FINAL_COMPILE_DATABASE}"
    --pcm-root "${BIOSEQ_SQL_PRODUCTION_PCM_ROOT}"
    --pcm-manifest "${BIOSEQ_SQL_PRODUCTION_FINAL_PCM_MANIFEST}"
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
    --output "${BIOSEQ_SQL_UPDATE_GOLDEN_INSTALL}"
    --manifest "${BIOSEQ_SQL_UPDATE_GOLDEN_MANIFEST}"
    --physical-ledger "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
    --require-checked-physical-ledger
    --golden "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq--1.0.sql"
    --stamp "${BIOSEQ_SQL_UPDATE_GOLDEN_STAMP}"
    --support-manifest "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
    --support-seal "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
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
    --row-codecs-output
      "storage_metadata=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_METADATA}"
    --row-codecs-output
      "storage_indexing_pg=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_INDEXING_PG}"
    --row-codecs-output
      "storage_load_pg=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_LOAD_PG}"
    --row-codecs-output
      "storage_manager=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_STORAGE_MANAGER}"
    --row-codecs-output
      "table=${BIOSEQ_SQL_PRODUCTION_ROW_CODECS_TABLE}"
    --fmgr-entrypoints-output
      "mod=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_MOD}"
    --fmgr-entrypoints-output
      "highway_target_test_support=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_HIGHWAY_TARGET_TEST_SUPPORT}"
    --fmgr-entrypoints-output
      "storage_manager_sequence_storage_manager=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_STORAGE_MANAGER}"
    --fmgr-entrypoints-output
      "table_access_hooks=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_TABLE_ACCESS_HOOKS}"
    --fmgr-entrypoints-output
      "storage_load_pg=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_STORAGE_LOAD_PG}"
    --fmgr-entrypoints-output
      "sequences_sample_table_udf=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_SAMPLE_TABLE_UDF}"
    --fmgr-entrypoints-output
      "types_pg_compact_sequence=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_COMPACT_SEQUENCE}"
    --fmgr-entrypoints-output
      "vector_types_integer=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_INTEGER}"
    --fmgr-entrypoints-output
      "vector_types_date=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_DATE}"
    --fmgr-entrypoints-output
      "vector_types_aggregates=${BIOSEQ_SQL_UPDATE_GOLDEN_FMGR_VECTOR_AGGREGATES}"
    --fmgr-entrypoints-output
      "vector_comparison_registry=${BIOSEQ_SQL_PRODUCTION_VECTOR_COMPARISON_REGISTRY}"
    --fmgr-entrypoints-output
      "vector_aggregate_registry=${BIOSEQ_SQL_PRODUCTION_VECTOR_AGGREGATE_REGISTRY}"
    --guc-parity-ledger "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
    --guc-output-root "${BIOSEQ_SQL_GUC_OUTPUT_ROOT}"
    --guc-active-profile production
    --require-complete-codegen
    --extension-schema public
  DEPENDS
    bioseq_codegen_project
    "${BIOSEQ_CODEGEN_EXECUTABLE}"
    "${BIOSEQ_CODEGEN_CLANG_BINARY}"
    "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
    "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
    "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    "${BIOSEQ_SQL_PRODUCTION_FINAL_PREPARE_STAMP}"
    "${BIOSEQ_SQL_PRODUCTION_FINAL_COMPILE_DATABASE}"
    "${BIOSEQ_SQL_PRODUCTION_FINAL_PCM_MANIFEST}"
    "${BIOSEQ_SQL_REGISTRY_PRODUCTION_FINAL_FILE}"
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"
    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}"
    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_MANIFEST}"
    ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES}
    ${BIOSEQ_SQL_PRODUCTION_SUPPORT_VECTOR_REGISTRY_MODULES}
    ${BIOSEQ_SQL_GUC_production_MODULES}
    ${BIOSEQ_SQL_GUC_test_indexes_MODULES}
    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"
    "${BIOSEQ_SQL_CORE_DESCRIPTOR_HASH}"
    "${BIOSEQ_SQL_CORE_MANIFEST}"
    ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
    "${BIOSEQ_SQL_VALIDATOR_SELF_TEST_STAMP}"
    "${BIOSEQ_SQL_CHECKED_PHYSICAL_LEDGER}"
    "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
    ${BIOSEQ_SQL_REGISTRY_PRODUCTION_FINAL_SOURCES}
  COMMENT
    "Maintainer-only update of generated bioseq--1.0.sql"
  VERBATIM)
add_dependencies(
  bioseq_sql_update_golden
  bioseq_sql_prepare_final_production)
endif()

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
    bioseq_format6_parallel_scan_probe_worker_main
  ${_bioseq_sql_completion_target_option}
  PG_ENTRYPOINT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/backend/pg/mod_impl.cppm"
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)
add_dependencies(postgresql_bioseq
  "${BIOSEQ_TYPED_GENERATION_TARGET}")

# Generated target primaries are the only exported providers. Their private
# binder implementation units and cold inventory are ordinary private sources.
# Algorithm partitions and the dispatch-processor registry are named module
# partition providers, so CMake must own them through a CXX_MODULES file set.
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES}
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS}
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY}"
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PROPERTIES GENERATED TRUE)
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_BINDERS}
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_sources(postgresql_bioseq
  PRIVATE
    ${BIOSEQ_HIGHWAY_GENERATED_TARGET_BINDERS}
    "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PRIVATE FILE_SET bioseq_highway_generated_primaries
    TYPE CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES
      ${BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES}
      ${BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS}
      "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY}")
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_highway_target_policy)
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
endif()
# The Bioseq DSO always owns the provider half of the cache rendezvous ABI and
# the small PG lifecycle helpers, even when the sibling cache DSO is disabled.
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_cache_provider_api
  bioseq_pg_support
  bioseq_pg_sql_boundary
  bioseq_sql_annotation_model)

# Proofs consume the generator-owned scan/modmap closure, while the DSO link
# must consume the completed proofs.  A concrete link dependency expresses
# that partial order without making every source scan wait on its own proof.
set(BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/postgresql-bioseq-link-proof.stamp")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CODEGEN_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP}"
  DEPENDS
    bioseq_sql_check_golden
    bioseq_sql_gcc_compile_contracts
  COMMENT
    "Sealing SQL generation and boundary proofs before linking bioseq"
  VERBATIM)
add_custom_target(bioseq_sql_postgresql_link_proof
  DEPENDS "${BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP}")
set_property(TARGET postgresql_bioseq APPEND PROPERTY LINK_DEPENDS
  "${BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP}")

# Keep only compile-readiness dependencies target-wide.  Proof targets belong
# to the link gate above; attaching them here would re-enter CXX.dd through the
# extension's .ddi object-order dependency.
add_dependencies(postgresql_bioseq bioseq_codegen_project)
add_dependencies(postgresql_bioseq bioseq_sql_annotation_probe)

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
if(BIOSEQ_TEST_INDEXES)
  target_compile_definitions(postgresql_bioseq PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(bioseq_modules_nopg PRIVATE BIOSEQ_TEST_INDEXES=1)
  if(TARGET bioseq_aligner_int_tests_exec)
    target_compile_definitions(bioseq_aligner_int_tests_exec PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
  if(TARGET bioseq_aligner_tests_exec)
    target_compile_definitions(bioseq_aligner_tests_exec PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
endif()

# DOC0388 catalog parity is a runtime gate, not a compilation side effect.
# The named target is available for an exact focused reproduction, while the
# integration executable invokes the same command during a full test run.
# Consequently build_only retains its compile-only contract.
get_target_property(
  BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE
  postgresql_bioseq
  BIOSEQ_EXTENSION_CONTROL_FILE)
if(NOT BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE
   OR BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE MATCHES "-NOTFOUND$")
  message(FATAL_ERROR
    "postgresql_bioseq did not publish its control file for catalog parity")
endif()
if(BIOSEQ_TEST_INDEXES)
  set(BIOSEQ_DOC0388_CATALOG_PARITY_ACTIVE_PROFILE test_indexes)
else()
  set(BIOSEQ_DOC0388_CATALOG_PARITY_ACTIVE_PROFILE production)
endif()
get_filename_component(
  BIOSEQ_DOC0388_CATALOG_PARITY_REPOSITORY_ROOT
  "${CMAKE_SOURCE_DIR}/.."
  ABSOLUTE)
set(BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/run_private_catalog_parity.py")
set(BIOSEQ_DOC0388_CATALOG_PARITY_STAGE_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/stage_private_postgres.cmake")
set(BIOSEQ_DOC0388_CATALOG_PARITY_CONFIG_TEMPLATE
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/private_postgresql.conf.in")
set(BIOSEQ_DOC0388_CATALOG_PARITY_SNAPSHOT_QUERY
  "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_catalog_snapshot.sql")
set(BIOSEQ_DOC0388_CATALOG_PARITY_APPROVED_DELTA
  "${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_catalog_delta.json")
set(BIOSEQ_DOC0388_CATALOG_PARITY_OUTPUT_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/catalog-parity")

set(BIOSEQ_DOC0388_CATALOG_PARITY_COMMAND
  "${Python3_EXECUTABLE}"
  "${BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT}"
  run
  --repository-root
    "${BIOSEQ_DOC0388_CATALOG_PARITY_REPOSITORY_ROOT}"
  --output-root
    "${BIOSEQ_DOC0388_CATALOG_PARITY_OUTPUT_ROOT}"
  --cmake
    "${CMAKE_COMMAND}"
  --git
    "${GIT_EXECUTABLE}"
  --pg-config
    "${PostgreSQL_CONFIG}"
  --stage-script
    "${BIOSEQ_DOC0388_CATALOG_PARITY_STAGE_SCRIPT}"
  --config-template
    "${BIOSEQ_DOC0388_CATALOG_PARITY_CONFIG_TEMPLATE}"
  --control-file
    "${BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE}"
  --candidate-sql
    "${BIOSEQ_SQL_GENERATED_INSTALL}"
  --candidate-manifest
    "${BIOSEQ_SQL_GENERATED_MANIFEST}"
  --candidate-dso
    "$<TARGET_FILE:postgresql_bioseq>"
  --snapshot-query
    "${BIOSEQ_DOC0388_CATALOG_PARITY_SNAPSHOT_QUERY}"
  --approved-delta
    "${BIOSEQ_DOC0388_CATALOG_PARITY_APPROVED_DELTA}"
  --guc-parity-ledger
    "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
  --active-profile
    "${BIOSEQ_DOC0388_CATALOG_PARITY_ACTIVE_PROFILE}"
  --timeout-seconds
    180)

add_custom_target(bioseq_doc0388_catalog_parity
  COMMAND "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${Python3_EXECUTABLE}"
    "${BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT}"
    self-test
  COMMAND "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    ${BIOSEQ_DOC0388_CATALOG_PARITY_COMMAND}
  DEPENDS
    postgresql_bioseq
    "${BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT}"
    "${BIOSEQ_DOC0388_CATALOG_PARITY_STAGE_SCRIPT}"
    "${BIOSEQ_DOC0388_CATALOG_PARITY_CONFIG_TEMPLATE}"
    "${BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE}"
    "${BIOSEQ_DOC0388_CATALOG_PARITY_SNAPSHOT_QUERY}"
    "${BIOSEQ_DOC0388_CATALOG_PARITY_APPROVED_DELTA}"
    "${BIOSEQ_SQL_GUC_PARITY_LEDGER}"
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/private_postgres.py"
    "${BIOSEQ_SQL_GENERATED_INSTALL}"
    "${BIOSEQ_SQL_GENERATED_MANIFEST}"
  COMMENT
    "Comparing immutable legacy and generated catalogs on clean PostgreSQL 17 nodes"
  USES_TERMINAL
  VERBATIM)

if(TARGET bioseq_aligner_int_tests_exec)
  add_dependencies(
    bioseq_aligner_int_tests_exec
    postgresql_bioseq)
  target_compile_definitions(bioseq_aligner_int_tests_exec PRIVATE
    "BIOSEQ_DOC0388_CATALOG_PARITY_PYTHON=\"${Python3_EXECUTABLE}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT=\"${BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_REPOSITORY_ROOT=\"${BIOSEQ_DOC0388_CATALOG_PARITY_REPOSITORY_ROOT}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_OUTPUT_ROOT=\"${BIOSEQ_DOC0388_CATALOG_PARITY_OUTPUT_ROOT}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_CMAKE=\"${CMAKE_COMMAND}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_GIT=\"${GIT_EXECUTABLE}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_PG_CONFIG=\"${PostgreSQL_CONFIG}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_STAGE_SCRIPT=\"${BIOSEQ_DOC0388_CATALOG_PARITY_STAGE_SCRIPT}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_CONFIG_TEMPLATE=\"${BIOSEQ_DOC0388_CATALOG_PARITY_CONFIG_TEMPLATE}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE=\"${BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_SQL=\"${BIOSEQ_SQL_GENERATED_INSTALL}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_MANIFEST=\"${BIOSEQ_SQL_GENERATED_MANIFEST}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_DSO=\"$<TARGET_FILE:postgresql_bioseq>\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_SNAPSHOT_QUERY=\"${BIOSEQ_DOC0388_CATALOG_PARITY_SNAPSHOT_QUERY}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_APPROVED_DELTA=\"${BIOSEQ_DOC0388_CATALOG_PARITY_APPROVED_DELTA}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_GUC_LEDGER=\"${BIOSEQ_SQL_GUC_PARITY_LEDGER}\""
    "BIOSEQ_DOC0388_CATALOG_PARITY_ACTIVE_PROFILE=\"${BIOSEQ_DOC0388_CATALOG_PARITY_ACTIVE_PROFILE}\"")
  if(BIOSEQ_TEST_INDEXES)
    target_compile_definitions(
      bioseq_aligner_int_tests_exec PRIVATE
      "BIOSEQ_DOC0388_ROW_AUTHORITY_LEDGER=\"${CMAKE_CURRENT_LIST_DIR}/testdata/doc0388_row_authority_migration_ledger.json\"")
    if(TARGET bioseq_doc0388_ledgers)
      add_dependencies(
        bioseq_aligner_int_tests_exec
        bioseq_doc0388_ledgers)
    endif()
  endif()
endif()

# Runtime ERROR evidence is an explicit, non-ALL gate. One build tree owns the
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
      "DOC0388 runtime evidence is not configured. Set "
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
      "DOC0388 case template must be an existing regular non-symlink file: "
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
      "${CMAKE_CURRENT_LIST_DIR}/integration_tests/safety_runtime_control.cc"
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
        "${CMAKE_CURRENT_LIST_DIR}/integration_tests/safety_runtime_harness_main.cc"
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
        "Sealed DOC0388 profile artifacts are configured, but the live "
        "dual-profile evidence gate requires "
        "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR."
      COMMAND "${CMAKE_COMMAND}" -E false
      USES_TERMINAL
      VERBATIM)
  else()
    if(BIOSEQ_TEST_INDEXES)
      message(FATAL_ERROR
        "DOC0388 aggregate evidence must run from the production profile "
        "with a sibling test-index build tree")
    endif()
    if(NOT BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE STREQUAL "isolated")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE must be isolated for "
        "dual-profile evidence")
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
      "${CMAKE_CURRENT_LIST_DIR}/../cmake/run_private_runtime_evidence.py")
    set(_bioseq_sql_runtime_stage_script
      "${CMAKE_CURRENT_LIST_DIR}/../cmake/stage_private_postgres.cmake")
    set(_bioseq_sql_runtime_config_template
      "${CMAKE_CURRENT_LIST_DIR}/../cmake/private_postgresql.conf.in")
    set(_bioseq_sql_runtime_producer
      "${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/runtime_evidence.py")
    file(REAL_PATH "${CMAKE_COMMAND}"
      _bioseq_sql_runtime_cmake_executable)
    if(NOT EXISTS "${_bioseq_sql_runtime_cmake_executable}"
       OR IS_DIRECTORY "${_bioseq_sql_runtime_cmake_executable}"
       OR IS_SYMLINK "${_bioseq_sql_runtime_cmake_executable}")
      message(FATAL_ERROR
        "DOC0388 runtime evidence requires CMake to resolve to a regular "
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
          "DOC0388 runtime evidence source is missing: "
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
      "${_bioseq_sql_runtime_candidate_root}/evidence-validation.json")

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
        "Publishing dual-profile PostgreSQL ERROR runtime evidence candidate"
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
        "Validating promoted dual-profile PostgreSQL ERROR runtime evidence"
      USES_TERMINAL
      VERBATIM)
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
# BIOSEQ_BUILD_EXTERNAL_API_CLIENTS is declared at the repository root so the
# isolated generator can be provisioned once before add_subdirectory(bioseq).
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  set(BIOSEQ_EXTERNAL_GENERATED_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/external")
  add_subdirectory(external)
  if(TARGET bioseq_external_integration_tests_exec)
    add_dependencies(
      bioseq_external_integration_tests_exec
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
  endif()
endif()
