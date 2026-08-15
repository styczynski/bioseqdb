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

function(_bioseq_build_config_profile profile ordinal_output
    all_alphabets_output all_coordinates_output adaptive_lengths_output
    extended_coordinates_output full_specialization_output
    scheduler_unit_count_output)
  set(_profiles
    dna4_uint64
    all_alphabets_uint64
    all_coordinates
    all_coordinates_adaptive_lengths
    extended_kmer_coordinates
    full_specialization)
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
