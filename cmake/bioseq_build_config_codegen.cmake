include_guard(GLOBAL)

function(_bioseq_build_config_fail message_text)
  message(FATAL_ERROR "Bioseq build config: ${message_text}")
endfunction()

function(_bioseq_build_config_bool value output_variable)
  if("${value}")
    set(${output_variable} 1 PARENT_SCOPE)
  else()
    set(${output_variable} 0 PARENT_SCOPE)
  endif()
endfunction()

function(_bioseq_build_config_cache_testing build_cache_value
    cache_testing_value output_variable)
  _bioseq_build_config_bool("${build_cache_value}" _build_cache)
  _bioseq_build_config_bool("${cache_testing_value}" _cache_testing)
  if(_build_cache AND _cache_testing)
    set(${output_variable} 1 PARENT_SCOPE)
  else()
    set(${output_variable} 0 PARENT_SCOPE)
  endif()
endfunction()

function(_bioseq_build_config_profiles output_variable)
  set(${output_variable}
    dna4_uint64
    all_alphabets_uint64
    all_coordinates
    all_coordinates_adaptive_lengths
    extended_kmer_coordinates
    full_specialization
    PARENT_SCOPE)
endfunction()

function(_bioseq_build_config_count_literal source_text needle output_variable)
  if("${needle}" STREQUAL "")
    _bioseq_build_config_fail("literal occurrence marker must not be empty")
  endif()
  string(LENGTH "${source_text}" _source_length)
  string(LENGTH "${needle}" _needle_length)
  string(REPLACE "${needle}" "" _source_without_needle "${source_text}")
  string(LENGTH "${_source_without_needle}" _source_without_needle_length)
  math(EXPR _occurrence_bytes
    "${_source_length} - ${_source_without_needle_length}")
  math(EXPR _occurrence_count
    "${_occurrence_bytes} / ${_needle_length}")
  set(${output_variable} "${_occurrence_count}" PARENT_SCOPE)
endfunction()

# Parse only the canonical profile declaration; this is not a general C++ parser.
function(_bioseq_build_config_parse_template_flatten_default source_label
    source_text ordinal_output profile_output)
  set(_declaration_name "bioseq_template_flatten_profile")
  set(_override_macro "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE")
  _bioseq_build_config_count_literal(
    "${source_text}" "${_declaration_name}" _declaration_name_count)
  _bioseq_build_config_count_literal(
    "${source_text}" "${_override_macro}" _override_macro_count)
  if(_declaration_name_count EQUAL 0 AND _override_macro_count EQUAL 0)
    _bioseq_build_config_fail(
      "${source_label} is missing the canonical template-flatten profile declaration")
  endif()
  if(_declaration_name_count GREATER 1 OR _override_macro_count GREATER 1)
    _bioseq_build_config_fail(
      "${source_label} must contain exactly one canonical template-flatten "
      "profile declaration; found ${_declaration_name_count} declaration names "
      "and ${_override_macro_count} override macros")
  endif()
  if(NOT _declaration_name_count EQUAL 1 OR NOT _override_macro_count EQUAL 1)
    _bioseq_build_config_fail(
      "${source_label} has a malformed template-flatten profile declaration")
  endif()

  set(_line_anchored_source "\n${source_text}")
  set(_declaration_start
    "\ninline constexpr unsigned int ${_declaration_name}")
  _bioseq_build_config_count_literal(
    "${_line_anchored_source}" "${_declaration_start}" _declaration_start_count)
  if(NOT _declaration_start_count EQUAL 1)
    _bioseq_build_config_fail(
      "${source_label} template-flatten profile must begin exactly one authored "
      "declaration line with 'inline constexpr unsigned int'")
  endif()

  _bioseq_build_config_profiles(_profiles)
  list(LENGTH _profiles _profile_count)
  if(_profile_count EQUAL 0)
    _bioseq_build_config_fail("the semantic profile catalog must not be empty")
  endif()
  math(EXPR _maximum_ordinal "${_profile_count} - 1")
  string(REGEX REPLACE "[ \t\r\n]+" "" _compact_source "${source_text}")
  string(CONCAT _declaration_prefix
    "inlineconstexprunsignedint${_declaration_name}"
    "[[bioseq::build_config(\"${_override_macro}\","
    "\"profile_ordinal\",\"0..${_maximum_ordinal}\")]]=")
  _bioseq_build_config_count_literal(
    "${_compact_source}" "${_declaration_prefix}" _declaration_prefix_count)
  if(NOT _declaration_prefix_count EQUAL 1)
    _bioseq_build_config_fail(
      "${source_label} template-flatten profile declaration must use unsigned int "
      "and the exact build-config attribute macro, unit, and domain '0..${_maximum_ordinal}'")
  endif()

  string(FIND "${_compact_source}" "${_declaration_prefix}" _declaration_offset)
  string(LENGTH "${_declaration_prefix}" _declaration_prefix_length)
  math(EXPR _initializer_offset
    "${_declaration_offset} + ${_declaration_prefix_length}")
  string(SUBSTRING "${_compact_source}" "${_initializer_offset}" -1
    _initializer_tail)
  string(FIND "${_initializer_tail}" ";" _initializer_terminator)
  if(_initializer_terminator LESS 0)
    _bioseq_build_config_fail(
      "${source_label} template-flatten profile declaration is missing its semicolon")
  endif()
  string(SUBSTRING "${_initializer_tail}" 0 "${_initializer_terminator}"
    _initializer)
  if(NOT "${_initializer}" MATCHES "^(0|[1-9][0-9]*)U$")
    _bioseq_build_config_fail(
      "${source_label} template-flatten profile default must use a canonical "
      "decimal unsigned literal such as 0U; got '${_initializer}'")
  endif()
  string(REGEX REPLACE "U$" "" _default_ordinal "${_initializer}")

  set(_default_profile "")
  set(_catalog_ordinal 0)
  foreach(_candidate_profile IN LISTS _profiles)
    if("${_default_ordinal}" STREQUAL "${_catalog_ordinal}")
      set(_default_profile "${_candidate_profile}")
    endif()
    math(EXPR _catalog_ordinal "${_catalog_ordinal} + 1")
  endforeach()
  if("${_default_profile}" STREQUAL "")
    _bioseq_build_config_fail(
      "${source_label} template-flatten profile default ordinal "
      "'${_default_ordinal}' is outside the authored domain '0..${_maximum_ordinal}'")
  endif()

  set(${ordinal_output} "${_default_ordinal}" PARENT_SCOPE)
  set(${profile_output} "${_default_profile}" PARENT_SCOPE)
endfunction()

function(_bioseq_build_config_template_flatten_default source_path
    ordinal_output profile_output)
  if(NOT IS_ABSOLUTE "${source_path}" OR NOT EXISTS "${source_path}"
      OR IS_DIRECTORY "${source_path}")
    _bioseq_build_config_fail(
      "authored build-config source must be an existing absolute file: '${source_path}'")
  endif()
  file(READ "${source_path}" _authored_source)
  _bioseq_build_config_parse_template_flatten_default(
    "${source_path}" "${_authored_source}" _default_ordinal _default_profile)
  set(${ordinal_output} "${_default_ordinal}" PARENT_SCOPE)
  set(${profile_output} "${_default_profile}" PARENT_SCOPE)
endfunction()

function(_bioseq_build_config_profile profile ordinal_output
    all_alphabets_output all_coordinates_output adaptive_lengths_output
    extended_coordinates_output full_specialization_output
    scheduler_unit_count_output)
  _bioseq_build_config_profiles(_profiles)
  list(FIND _profiles "${profile}" _profile_ordinal)
  if(_profile_ordinal EQUAL -1)
    _bioseq_build_config_fail(
      "unsupported semantic profile '${profile}'")
  endif()
  if(_profile_ordinal GREATER_EQUAL 1)
    set(${all_alphabets_output} 1 PARENT_SCOPE)
  else()
    set(${all_alphabets_output} 0 PARENT_SCOPE)
  endif()
  if(_profile_ordinal GREATER_EQUAL 2)
    set(${all_coordinates_output} 1 PARENT_SCOPE)
  else()
    set(${all_coordinates_output} 0 PARENT_SCOPE)
  endif()
  if(_profile_ordinal GREATER_EQUAL 3)
    set(${adaptive_lengths_output} 1 PARENT_SCOPE)
  else()
    set(${adaptive_lengths_output} 0 PARENT_SCOPE)
  endif()
  if(_profile_ordinal GREATER_EQUAL 4)
    set(${extended_coordinates_output} 1 PARENT_SCOPE)
  else()
    set(${extended_coordinates_output} 0 PARENT_SCOPE)
  endif()
  if(_profile_ordinal GREATER_EQUAL 5)
    set(${full_specialization_output} 1 PARENT_SCOPE)
  else()
    set(${full_specialization_output} 0 PARENT_SCOPE)
  endif()
  if(_profile_ordinal EQUAL 0)
    set(_scheduler_unit_count 7)
  elseif(_profile_ordinal EQUAL 1)
    set(_scheduler_unit_count 14)
  else()
    set(_scheduler_unit_count 56)
  endif()
  set(${ordinal_output} "${_profile_ordinal}" PARENT_SCOPE)
  set(${scheduler_unit_count_output} "${_scheduler_unit_count}" PARENT_SCOPE)
endfunction()

function(bioseq_add_build_config_module)
  set(_one_value_arguments
    TARGET SOURCE OUTPUT_ROOT PROFILE TEST_INDEXES CACHE_TESTING
    EXTERNAL_API_CLIENTS
    VACUUM_WAIT_MILLISECONDS VACUUM_TIMEOUT_MILLISECONDS
    OUT_MODULE OUT_COMPILE_CONTRACT OUT_MANIFEST OUT_STAMP
    OUT_SCHEDULER_UNIT_COUNT)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${_one_value_arguments}" "")
  foreach(_required IN ITEMS TARGET SOURCE OUTPUT_ROOT PROFILE TEST_INDEXES
      CACHE_TESTING EXTERNAL_API_CLIENTS VACUUM_WAIT_MILLISECONDS
      VACUUM_TIMEOUT_MILLISECONDS
      OUT_MODULE OUT_COMPILE_CONTRACT OUT_MANIFEST OUT_STAMP
      OUT_SCHEDULER_UNIT_COUNT)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      _bioseq_build_config_fail(
        "bioseq_add_build_config_module requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_build_config_fail(
      "unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(TARGET "${ARG_TARGET}")
    _bioseq_build_config_fail("target '${ARG_TARGET}' already exists")
  endif()
  if(NOT TARGET bioseq_codegen_project OR NOT TARGET bioseq_codegen)
    _bioseq_build_config_fail(
      "the shared bioseq_codegen host tool must exist before build-config generation")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_SOURCE}" OR NOT EXISTS "${ARG_SOURCE}")
    _bioseq_build_config_fail(
      "SOURCE must be an existing absolute file: '${ARG_SOURCE}'")
  endif()
  file(REAL_PATH "${ARG_SOURCE}" _source)
  cmake_path(NORMAL_PATH ARG_SOURCE OUTPUT_VARIABLE _normalized_source)
  if(NOT _source STREQUAL _normalized_source)
    _bioseq_build_config_fail(
      "SOURCE must use its canonical non-symlink spelling")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_OUTPUT_ROOT}")
    _bioseq_build_config_fail("OUTPUT_ROOT must be absolute")
  endif()
  cmake_path(NORMAL_PATH ARG_OUTPUT_ROOT OUTPUT_VARIABLE _output_root)
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_output_root}"
    NORMALIZE _inside_build)
  if(NOT _inside_build OR _output_root STREQUAL CMAKE_BINARY_DIR)
    _bioseq_build_config_fail(
      "OUTPUT_ROOT must be an owned build-tree subdirectory")
  endif()

  _bioseq_build_config_profile("${ARG_PROFILE}"
    _profile_ordinal
    _all_alphabets
    _all_coordinates
    _adaptive_lengths
    _extended_coordinates
    _full_specialization
    _scheduler_heavy_units)
  _bioseq_build_config_bool("${ARG_TEST_INDEXES}" _test_indexes)
  _bioseq_build_config_bool("${ARG_CACHE_TESTING}" _cache_testing)
  _bioseq_build_config_bool(
    "${ARG_EXTERNAL_API_CLIENTS}" _external_api_clients)

  set(_module "${_output_root}/bioseq_build_config.cppm")
  set(_compile_contract
    "${_output_root}/bioseq_build_config_compile_profile.cpp")
  set(_manifest "${_output_root}/bioseq_build_config.manifest.json")
  set(_stamp "${_output_root}/bioseq_build_config.stamp")
  set(_all_alphabets_override
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64=${_all_alphabets}")
  set(_extended_coordinates_override
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES=${_extended_coordinates}")
  string(CONCAT _vacuum_wait_override
    "BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS="
    "${ARG_VACUUM_WAIT_MILLISECONDS}")
  string(CONCAT _vacuum_timeout_override
    "BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS="
    "${ARG_VACUUM_TIMEOUT_MILLISECONDS}")
  set_source_files_properties(
    "${_module}" "${_compile_contract}" PROPERTIES GENERATED TRUE)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_source}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")

  add_custom_command(
    OUTPUT "${_module}" "${_compile_contract}" "${_manifest}" "${_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_root}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>" build_config
      --source "${_source}"
      --output "${_module}"
      --compile-contract "${_compile_contract}"
      --manifest "${_manifest}"
      --stamp "${_stamp}"
      --override
        "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE=${_profile_ordinal}"
      --override
        "${_all_alphabets_override}"
      --override
        "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES=${_all_coordinates}"
      --override
        "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS=${_adaptive_lengths}"
      --override
        "${_extended_coordinates_override}"
      --override
        "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION=${_full_specialization}"
      --override
        "BIOSEQ_BUILD_CONFIG_SCHEDULER_TYPED_HEAVY_UNIT_COUNT=${_scheduler_heavy_units}"
      --override
        "BIOSEQ_BUILD_CONFIG_TEST_INDEXES_ENABLED=${_test_indexes}"
      --override
        "BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED=${_cache_testing}"
      --override
        "BIOSEQ_BUILD_CONFIG_EXTERNAL_API_CLIENTS_ENABLED=${_external_api_clients}"
      --override
        "${_vacuum_wait_override}"
      --override
        "${_vacuum_timeout_override}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${_source}"
      "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    COMMENT "Generating canonical Bioseq build-configuration module"
    VERBATIM)

  add_custom_target("${ARG_TARGET}_generate"
    DEPENDS "${_module}" "${_compile_contract}" "${_manifest}" "${_stamp}")
  add_dependencies("${ARG_TARGET}_generate" bioseq_codegen_project)

  add_library("${ARG_TARGET}" STATIC)
  add_dependencies("${ARG_TARGET}" "${ARG_TARGET}_generate")
  set_target_properties("${ARG_TARGET}" PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON)
  target_sources("${ARG_TARGET}"
    PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${_output_root}"
      FILES "${_module}")

  add_library("${ARG_TARGET}_compile_profile_contract" OBJECT
    "${_compile_contract}")
  add_dependencies(
    "${ARG_TARGET}_compile_profile_contract" "${ARG_TARGET}_generate")
  set_target_properties("${ARG_TARGET}_compile_profile_contract" PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(
    "${ARG_TARGET}_compile_profile_contract" PRIVATE "${ARG_TARGET}")

  set(${ARG_OUT_MODULE} "${_module}" PARENT_SCOPE)
  set(${ARG_OUT_COMPILE_CONTRACT} "${_compile_contract}" PARENT_SCOPE)
  set(${ARG_OUT_MANIFEST} "${_manifest}" PARENT_SCOPE)
  set(${ARG_OUT_STAMP} "${_stamp}" PARENT_SCOPE)
  set(${ARG_OUT_SCHEDULER_UNIT_COUNT}
    "${_scheduler_heavy_units}" PARENT_SCOPE)
endfunction()
