# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS ${VCPKG_CMAKE_CONFIGURE_OPTIONS} -DSQLGEN_POSTGRES=1)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

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
set(BIOSEQ_PG_RUNTIME_OWNER
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/supervisor_pg.cppm)
foreach(_bioseq_pg_source IN LISTS BIOSEQ_PG_RUNTIME_GUARD_SOURCES)
  if("${_bioseq_pg_source}" STREQUAL "${BIOSEQ_PG_RUNTIME_OWNER}")
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

# DOC0377 — representation-erased sequence consumers must not reach through
# the lazy facade into a concrete scanner/range implementation.  These files
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
        "DOC0377 concrete lazy/scanner dependency in ${_bioseq_lazy_source}; "
        "consume basic_lazy_sequence_view or update the documented ownership boundary; "
        "matched code: ${_bioseq_lazy_code_line}")
    endif()
  endforeach()
endforeach()

# DOC0377 §14.2 completion gate.  The broad boundary gate above prevents a
# concrete lazy/scanner type from escaping anywhere in backend code; this
# narrower production-owner sweep also rejects the physical-geometry escape
# hatches named by the specification.
set(BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/pipe.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pipeline/minimap_port.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/block_aligner.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/alignment_hwy/ksw2_batched.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/minimizers_vulkan.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/storage/lazy_compact_sequence_batch_processor.cppm
  ${CMAKE_CURRENT_LIST_DIR}/backend/pg/scheduler/dispatch_processor_typed_impl.inc)
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
# sentinels in the two large minimizer modules.  The state machine rejects
# malformed, nested, duplicate, unknown, mismatched, missing, and unclosed
# scopes.  An exact line is then checked against both its source and active
# private resident/admitted scope, so copying an allowed spelling elsewhere is
# still a configure-time failure.
function(bioseq_doc0377_private_packed_scope_names
    _bioseq_source _bioseq_result)
  set(_bioseq_scopes)
  if("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers.cppm")
    set(_bioseq_scopes
      bounded_u32_dna4_view
      run_packed_kernel
      collect_minimizers_packed_paper_nthash_simd_checked
      consume_two_stack_nthash_fragment
      collect_minimizers_two_stack_nthash_exact_range_resident_impl_checked)
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
function(bioseq_doc0377_private_packed_scope_transition
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

function(bioseq_doc0377_private_packed_line_allowed
    _bioseq_source _bioseq_scope _bioseq_line _bioseq_result)
  set(_bioseq_allowed FALSE)
  if("${_bioseq_source}" STREQUAL
      "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers.cppm")
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
    elseif("${_bioseq_scope}" STREQUAL
        "consume_two_stack_nthash_fragment"
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
function(bioseq_doc0377_read_encoded_physical_lines
    _bioseq_source _bioseq_result)
  file(READ "${_bioseq_source}" _bioseq_contents)
  string(REPLACE "\r\n" "\n" _bioseq_contents "${_bioseq_contents}")
  string(REPLACE "\r" "\n" _bioseq_contents "${_bioseq_contents}")
  set(_bioseq_backslash_token
    "__BIOSEQ_DOC0377_LINE_ESCAPE_BACKSLASH_9F2D__")
  set(_bioseq_semicolon_token
    "__BIOSEQ_DOC0377_LINE_ESCAPE_SEMICOLON_9F2D__")
  set(_bioseq_open_bracket_token
    "__BIOSEQ_DOC0377_LINE_ESCAPE_OPEN_BRACKET_9F2D__")
  set(_bioseq_close_bracket_token
    "__BIOSEQ_DOC0377_LINE_ESCAPE_CLOSE_BRACKET_9F2D__")
  foreach(_bioseq_escape_token IN ITEMS
      "${_bioseq_backslash_token}"
      "${_bioseq_semicolon_token}"
      "${_bioseq_open_bracket_token}"
      "${_bioseq_close_bracket_token}")
    string(FIND "${_bioseq_contents}" "${_bioseq_escape_token}"
      _bioseq_escape_collision)
    if(NOT _bioseq_escape_collision EQUAL -1)
      message(FATAL_ERROR
        "DOC0377 physical-line escape token collision in ${_bioseq_source}")
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

function(bioseq_doc0377_decode_physical_line
    _bioseq_encoded _bioseq_result)
  set(_bioseq_line "${_bioseq_encoded}")
  string(REPLACE "__BIOSEQ_DOC0377_LINE_ESCAPE_SEMICOLON_9F2D__" ";"
    _bioseq_line "${_bioseq_line}")
  string(REPLACE "__BIOSEQ_DOC0377_LINE_ESCAPE_OPEN_BRACKET_9F2D__" "["
    _bioseq_line "${_bioseq_line}")
  string(REPLACE "__BIOSEQ_DOC0377_LINE_ESCAPE_CLOSE_BRACKET_9F2D__" "]"
    _bioseq_line "${_bioseq_line}")
  string(REPLACE "__BIOSEQ_DOC0377_LINE_ESCAPE_BACKSLASH_9F2D__" "\\"
    _bioseq_line "${_bioseq_line}")
  set(${_bioseq_result} "${_bioseq_line}" PARENT_SCOPE)
endfunction()

# Configure-time negative self-checks for the marker state machine and the
# source+scope+exact-line binding.  These do not touch production files.
set(_bioseq_doc0377_self_allowed alpha beta)
bioseq_doc0377_private_packed_scope_transition(
  "" "" "${_bioseq_doc0377_self_allowed}" BEGIN alpha
  _bioseq_self_active _bioseq_self_seen _bioseq_self_error)
if(NOT _bioseq_self_error STREQUAL "" OR NOT _bioseq_self_active STREQUAL "alpha")
  message(FATAL_ERROR "DOC0377 private-scope guard positive self-check failed")
endif()
bioseq_doc0377_private_packed_scope_transition(
  "${_bioseq_self_active}" "${_bioseq_self_seen}"
  "${_bioseq_doc0377_self_allowed}" BEGIN beta
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_nested)
bioseq_doc0377_private_packed_scope_transition(
  "${_bioseq_self_active}" "${_bioseq_self_seen}"
  "${_bioseq_doc0377_self_allowed}" END beta
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_mismatch)
bioseq_doc0377_private_packed_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_doc0377_self_allowed}" END alpha
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_unbalanced)
bioseq_doc0377_private_packed_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_doc0377_self_allowed}" BEGIN alpha
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_duplicate)
bioseq_doc0377_private_packed_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_doc0377_self_allowed}" BEGIN gamma
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_unknown)
bioseq_doc0377_private_packed_scope_transition(
  "" "${_bioseq_self_seen}" "${_bioseq_doc0377_self_allowed}" OTHER alpha
  _bioseq_self_ignored_active _bioseq_self_ignored_seen _bioseq_self_kind)
if(_bioseq_self_nested STREQUAL "" OR _bioseq_self_mismatch STREQUAL ""
    OR _bioseq_self_unbalanced STREQUAL "" OR _bioseq_self_duplicate STREQUAL ""
    OR _bioseq_self_unknown STREQUAL "" OR _bioseq_self_kind STREQUAL "")
  message(FATAL_ERROR "DOC0377 private-scope guard negative self-check failed")
endif()
set(_bioseq_doc0377_self_source
  "${CMAKE_CURRENT_LIST_DIR}/backend/sequences/kmers/two_stacks_minimizers.cppm")
bioseq_doc0377_private_packed_line_allowed(
  "${_bioseq_doc0377_self_source}" bounded_u32_dna4_view
  "return local_view_units * traits::letters_per_unit - local_bp_size;"
  _bioseq_self_exact_allowed)
bioseq_doc0377_private_packed_line_allowed(
  "${_bioseq_doc0377_self_source}" run_packed_kernel
  "return local_view_units * traits::letters_per_unit - local_bp_size;"
  _bioseq_self_wrong_scope_allowed)
bioseq_doc0377_private_packed_line_allowed(
  "${_bioseq_doc0377_self_source}" bounded_u32_dna4_view
  "return traits::letters_per_unit;" _bioseq_self_inexact_allowed)
if(NOT _bioseq_self_exact_allowed OR _bioseq_self_wrong_scope_allowed
    OR _bioseq_self_inexact_allowed)
  message(FATAL_ERROR "DOC0377 private-scope exact-line self-check failed")
endif()

foreach(_bioseq_facade_source IN LISTS
    BIOSEQ_LAZY_FACADE_COMPLETION_GUARD_SOURCES)
  list(FIND BIOSEQ_LAZY_CONCRETE_FIXTURE_ALLOWLIST
    "${_bioseq_facade_source}" _bioseq_facade_fixture_index)
  if(NOT _bioseq_facade_fixture_index EQUAL -1)
    continue()
  endif()
  bioseq_doc0377_private_packed_scope_names(
    "${_bioseq_facade_source}" _bioseq_facade_allowed_scopes)
  set(_bioseq_facade_active_scope "")
  set(_bioseq_facade_seen_scopes)
  set(_bioseq_facade_seen_token_hashes)
  set(_bioseq_facade_marker_count 0)
  bioseq_doc0377_read_encoded_physical_lines(
    "${_bioseq_facade_source}" _bioseq_facade_encoded_lines)
  foreach(_bioseq_facade_encoded_line IN LISTS
      _bioseq_facade_encoded_lines)
    bioseq_doc0377_decode_physical_line(
      "${_bioseq_facade_encoded_line}" _bioseq_facade_source_line)
    string(FIND "${_bioseq_facade_source_line}"
      "BIOSEQ_DOC0377_PRIVATE_PACKED_" _bioseq_facade_marker_offset)
    if(NOT _bioseq_facade_marker_offset EQUAL -1)
      string(STRIP "${_bioseq_facade_source_line}" _bioseq_facade_marker_line)
      if(_bioseq_facade_marker_line MATCHES
          "^//[ \t]*BIOSEQ_DOC0377_PRIVATE_PACKED_(BEGIN|END)[ \t]+([A-Za-z][A-Za-z0-9_]*)[ \t]*$")
        set(_bioseq_facade_marker_kind "${CMAKE_MATCH_1}")
        set(_bioseq_facade_marker_name "${CMAKE_MATCH_2}")
      else()
        message(FATAL_ERROR
          "DOC0377 malformed private packed scope marker in ${_bioseq_facade_source}: "
          "${_bioseq_facade_marker_line}")
      endif()
      bioseq_doc0377_private_packed_scope_transition(
        "${_bioseq_facade_active_scope}" "${_bioseq_facade_seen_scopes}"
        "${_bioseq_facade_allowed_scopes}"
        "${_bioseq_facade_marker_kind}" "${_bioseq_facade_marker_name}"
        _bioseq_facade_active_scope _bioseq_facade_seen_scopes
        _bioseq_facade_marker_error)
      if(NOT _bioseq_facade_marker_error STREQUAL "")
        message(FATAL_ERROR
          "DOC0377 ${_bioseq_facade_marker_error} in ${_bioseq_facade_source}")
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
      bioseq_doc0377_private_packed_line_allowed(
        "${_bioseq_facade_source}" "${_bioseq_facade_active_scope}"
        "${_bioseq_facade_code_line}"
        _bioseq_facade_private_allowed)
      if(NOT _bioseq_facade_private_allowed)
        message(FATAL_ERROR
          "DOC0377 facade completion dependency in ${_bioseq_facade_source}; "
          "physical scanner/packed access is permitted only in a named private "
          "resident/admitted kernel; matched code: ${_bioseq_facade_code_line}")
      endif()
      string(SHA256 _bioseq_facade_token_hash
        "${_bioseq_facade_active_scope}|${_bioseq_facade_code_line}")
      list(FIND _bioseq_facade_seen_token_hashes
        "${_bioseq_facade_token_hash}" _bioseq_facade_token_hash_index)
      if(NOT _bioseq_facade_token_hash_index EQUAL -1)
        message(FATAL_ERROR
          "DOC0377 duplicate exact private packed token in scope "
          "${_bioseq_facade_active_scope} in ${_bioseq_facade_source}: "
          "${_bioseq_facade_code_line}")
      endif()
      list(APPEND _bioseq_facade_seen_token_hashes
        "${_bioseq_facade_token_hash}")
    endif()
  endforeach()
  if(NOT _bioseq_facade_active_scope STREQUAL "")
    message(FATAL_ERROR
      "DOC0377 unclosed private packed scope '${_bioseq_facade_active_scope}' "
      "in ${_bioseq_facade_source}")
  endif()
  list(LENGTH _bioseq_facade_allowed_scopes
    _bioseq_facade_allowed_scope_count)
  math(EXPR _bioseq_facade_expected_marker_count
    "${_bioseq_facade_allowed_scope_count} * 2")
  if(NOT _bioseq_facade_marker_count EQUAL
      _bioseq_facade_expected_marker_count)
    message(FATAL_ERROR
      "DOC0377 private packed marker count mismatch in ${_bioseq_facade_source}; "
      "expected=${_bioseq_facade_expected_marker_count} "
      "actual=${_bioseq_facade_marker_count}")
  endif()
  foreach(_bioseq_facade_required_scope IN LISTS
      _bioseq_facade_allowed_scopes)
    list(FIND _bioseq_facade_seen_scopes
      "${_bioseq_facade_required_scope}" _bioseq_facade_required_scope_index)
    if(_bioseq_facade_required_scope_index EQUAL -1)
      message(FATAL_ERROR
        "DOC0377 missing private packed scope '${_bioseq_facade_required_scope}' "
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

# The typed dispatch implementation is split into one translation unit per
# alphabet/storage-width/scan-shape combination. Levels 0 and 1 must prune
# those files from the target itself; leaving them in SRC_FILES would still
# pay the module-import and template parsing cost even if their bodies were
# preprocessor-disabled.
set(BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCES
  ${BIOSEQ_DISPATCH_PROCESSOR_TYPED_SOURCES})
if(BIOSEQ_OPTTIMIZATION_LEVEL STREQUAL "0")
  list(FILTER BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCES
    INCLUDE REGEX "_dna4_w8(_|\\.cpp$)")
elseif(BIOSEQ_OPTTIMIZATION_LEVEL STREQUAL "1")
  list(FILTER BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCES
    INCLUDE REGEX "_dna[45]_w8(_|\\.cpp$)")
endif()
list(REMOVE_ITEM SRC_FILES ${BIOSEQ_DISPATCH_PROCESSOR_TYPED_SOURCES})
list(APPEND SRC_FILES ${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCES})
list(LENGTH BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCES
  BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT)
message(STATUS
  "bioseq: enabled ${BIOSEQ_DISPATCH_PROCESSOR_ENABLED_TYPED_SOURCE_COUNT} "
  "typed dispatch processor translation units")

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
  PG_ENTRYPOINT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/backend/pg/mod.cppm"
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)

# The Bioseq DSO always owns the provider half of the cache rendezvous ABI and
# the small PG lifecycle helpers, even when the sibling cache DSO is disabled.
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_cache_provider_api
  bioseq_pg_support)

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
  target_compile_definitions(bioseq_modules_nopg PRIVATE BIOSEQ_TEST_INDEXES=1)
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
# BIOSEQ_BUILD_EXTERNAL_API_CLIENTS is declared at the repository root so the
# isolated generator can be provisioned once before add_subdirectory(bioseq).
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
