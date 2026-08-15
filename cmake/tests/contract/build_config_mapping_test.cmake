cmake_minimum_required(VERSION 3.24)

get_filename_component(
  _repository_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${_repository_root}/cmake/bioseq_build_config_codegen.cmake")
include("${_repository_root}/cmake/bioseq_template_flatten_codegen.cmake")

if(DEFINED BIOSEQ_BUILD_CONFIG_NEGATIVE_CASE)
  if(BIOSEQ_BUILD_CONFIG_NEGATIVE_CASE STREQUAL "profile")
    _bioseq_build_config_profile(
      invalid_profile ordinal alphabets coordinates adaptive extended full units)
  else()
    message(FATAL_ERROR
      "Unknown build-config negative case '${BIOSEQ_BUILD_CONFIG_NEGATIVE_CASE}'")
  endif()
  message(FATAL_ERROR "Build-config negative case unexpectedly succeeded")
endif()

function(_require_equal actual expected context)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "${context}: expected '${expected}', got '${actual}'")
  endif()
endfunction()

function(_require_contains text needle context)
  string(FIND "${text}" "${needle}" _offset)
  if(_offset LESS 0)
    message(FATAL_ERROR "${context}: missing '${needle}'")
  endif()
endfunction()

function(_verify_profile profile expected_ordinal expected_alphabets
    expected_coordinates expected_adaptive expected_extended expected_full
    expected_units)
  _bioseq_build_config_profile("${profile}"
    actual_ordinal
    actual_alphabets
    actual_coordinates
    actual_adaptive
    actual_extended
    actual_full
    actual_units)
  _require_equal("${actual_ordinal}" "${expected_ordinal}"
    "${profile} ordinal")
  _require_equal("${actual_alphabets}" "${expected_alphabets}"
    "${profile} all-alphabets capability")
  _require_equal("${actual_coordinates}" "${expected_coordinates}"
    "${profile} all-coordinates capability")
  _require_equal("${actual_adaptive}" "${expected_adaptive}"
    "${profile} adaptive-lengths capability")
  _require_equal("${actual_extended}" "${expected_extended}"
    "${profile} extended-coordinates capability")
  _require_equal("${actual_full}" "${expected_full}"
    "${profile} full-specialization capability")
  _require_equal("${actual_units}" "${expected_units}"
    "${profile} scheduler unit count")
endfunction()

function(_verify_boolean spelling expected)
  _bioseq_build_config_bool("${spelling}" actual)
  _require_equal("${actual}" "${expected}"
    "CMake boolean '${spelling}'")
endfunction()

function(_verify_cache_testing build_cache cache_testing expected)
  _bioseq_build_config_cache_testing(
    "${build_cache}" "${cache_testing}" actual)
  _require_equal("${actual}" "${expected}"
    "cache-testing mapping for build-cache=${build_cache}, testing=${cache_testing}")
endfunction()

function(_verify_profile_enabled ordinal alphabet width expected)
  _bioseq_template_flatten_profile_enabled(
    "${ordinal}" "${alphabet}" "${width}" actual)
  _require_equal("${actual}" "${expected}"
    "template profile ${ordinal}, alphabet=${alphabet}, width=${width}")
endfunction()

_bioseq_build_config_profiles(_profiles)
set(_expected_profiles
  dna4_uint64
  all_alphabets_uint64
  all_coordinates
  all_coordinates_adaptive_lengths
  extended_kmer_coordinates
  full_specialization)
_require_equal("${_profiles}" "${_expected_profiles}"
  "canonical template-profile order")

_verify_profile(dna4_uint64 0 0 0 0 0 0 7)
_verify_profile(all_alphabets_uint64 1 1 0 0 0 0 14)
_verify_profile(all_coordinates 2 1 1 0 0 0 56)
_verify_profile(all_coordinates_adaptive_lengths 3 1 1 1 0 0 56)
_verify_profile(extended_kmer_coordinates 4 1 1 1 1 0 56)
_verify_profile(full_specialization 5 1 1 1 1 1 56)

_verify_profile_enabled(0 dna4 w8 TRUE)
_verify_profile_enabled(0 dna5 w8 FALSE)
_verify_profile_enabled(0 dna4 w4 FALSE)
_verify_profile_enabled(1 dna5 w8 TRUE)
_verify_profile_enabled(1 aminoacid w8 FALSE)
_verify_profile_enabled(1 dna4 w4 FALSE)
_verify_profile_enabled(2 aminoacid w4 TRUE)
_verify_profile_enabled(5 aminoacid w4 TRUE)

foreach(_true_value IN ITEMS 1 ON TRUE YES Y 2 -1)
  _verify_boolean("${_true_value}" 1)
endforeach()
foreach(_false_value IN ITEMS 0 OFF FALSE NO N IGNORE NOTFOUND invalid
    value-NOTFOUND)
  _verify_boolean("${_false_value}" 0)
endforeach()
_verify_boolean("" 0)

_verify_cache_testing(OFF OFF 0)
_verify_cache_testing(OFF ON 0)
_verify_cache_testing(ON OFF 0)
_verify_cache_testing(ON ON 1)
_verify_cache_testing(TRUE YES 1)
_verify_cache_testing(invalid ON 0)

foreach(_scheduler_case IN ITEMS
    "dna4_uint64|7|6"
    "all_alphabets_uint64|14|13"
    "all_coordinates|56|55"
    "all_coordinates_adaptive_lengths|56|55"
    "extended_kmer_coordinates|56|55"
    "full_specialization|56|55")
  string(REPLACE "|" ";" _scheduler_fields "${_scheduler_case}")
  list(GET _scheduler_fields 0 _scheduler_profile)
  list(GET _scheduler_fields 1 _scheduler_expected)
  list(GET _scheduler_fields 2 _scheduler_actual)
  _bioseq_template_flatten_scheduler_count_error(
    "${_scheduler_profile}" "${_scheduler_expected}" "${_scheduler_actual}"
    _scheduler_error)
  string(CONCAT _scheduler_expected_error
    "scheduler profile ${_scheduler_profile} must enable exactly "
    "${_scheduler_expected} heavy units; got ${_scheduler_actual}")
  _require_equal("${_scheduler_error}" "${_scheduler_expected_error}"
    "${_scheduler_profile} scheduler-count diagnostic")
endforeach()

set(_authored_config
  "${_repository_root}/bioseq/backend/common/bioseq_build_config.cppm")
file(READ "${_authored_config}" _authored_config_source)
set(_authored_annotation_marker "[[bioseq::build_config(")
string(LENGTH "${_authored_config_source}" _authored_source_length)
string(LENGTH "${_authored_annotation_marker}" _authored_marker_length)
string(REPLACE "${_authored_annotation_marker}" "" _authored_without_annotations
  "${_authored_config_source}")
string(LENGTH "${_authored_without_annotations}" _authored_without_annotations_length)
math(EXPR _authored_annotation_bytes
  "${_authored_source_length} - ${_authored_without_annotations_length}")
math(EXPR _authored_annotation_count
  "${_authored_annotation_bytes} / ${_authored_marker_length}")
_require_equal("${_authored_annotation_count}" 12
  "canonical authored build-config declaration count")
if("${_authored_config_source}" MATCHES "(^|\n)[ \t]*#")
  message(FATAL_ERROR
    "Canonical authored build config contains a preprocessor directive")
endif()
_require_contains("${_authored_config_source}"
  "inline constexpr bool bioseq_cache_testing_enabled"
  "canonical cache-testing declaration")
_require_contains("${_authored_config_source}"
  "BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED"
  "canonical cache-testing override macro")

file(READ "${_repository_root}/bioseq/CMakeLists.txt" _bioseq_cmake_source)
_require_contains("${_bioseq_cmake_source}"
  "_bioseq_build_config_profiles(BIOSEQ_TEMPLATE_FLATTEN_PROFILES)"
  "canonical template-profile catalog")
_require_contains("${_bioseq_cmake_source}"
  "_bioseq_build_config_profile("
  "canonical template-profile mapping")
if("${_bioseq_cmake_source}" MATCHES
    "BIOSEQ_TEMPLATE_FLATTEN_PROFILE[ \t\r\n]+STREQUAL")
  message(FATAL_ERROR
    "bioseq/CMakeLists.txt duplicates the canonical template-profile map")
endif()
_require_contains("${_bioseq_cmake_source}"
  "option(BIOSEQ_TEST_INDEXES
  \"Compile the test-only indexing descriptors into the extension + test execs\"
  OFF)"
  "test-indexes compatibility option")
_require_contains("${_bioseq_cmake_source}"
  "set(BIOSEQ_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS \"50\""
  "vacuum retry default")
_require_contains("${_bioseq_cmake_source}"
  "set(BIOSEQ_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS \"4500\""
  "vacuum timeout default")
string(FIND "${_bioseq_cmake_source}"
  "backend/common/bioseq_build_config.cppm" _authored_source_exclusion)
string(FIND "${_bioseq_cmake_source}"
  "bioseq_add_build_config_module(" _generator_registration)
if(_authored_source_exclusion LESS 0
    OR _generator_registration LESS 0
    OR _authored_source_exclusion GREATER _generator_registration)
  message(FATAL_ERROR
    "Authored build config is not excluded before generated-module registration")
endif()

file(READ
  "${_repository_root}/bioseq/backend/pg/include/bioseq/pg_build_profile.h"
  _profile_header_source)
_require_contains("${_profile_header_source}"
  "#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE dna4_uint64"
  "checked-in template-flatten default")
file(READ "${_repository_root}/bioseq_caching/CMakeLists.txt"
  _cache_cmake_source)
_require_contains("${_cache_cmake_source}"
  "option(BIOSEQ_CACHE_TESTING
  \"Build the noninstalled bioseq_cache test-support DSO\" OFF)"
  "cache-testing compatibility option")
file(READ "${_repository_root}/CMakeLists.txt" _root_cmake_source)
_require_contains("${_root_cmake_source}"
  "option(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
  \"Configure generated external API clients and their integration tests\" OFF)"
  "external-API compatibility option")

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -DBIOSEQ_BUILD_CONFIG_NEGATIVE_CASE=profile
    -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE _negative_result
  OUTPUT_VARIABLE _negative_output
  ERROR_VARIABLE _negative_error)
if(_negative_result EQUAL 0)
  message(FATAL_ERROR "Invalid build-config profile was accepted")
endif()
string(CONCAT _negative_diagnostics
  "${_negative_output}" "${_negative_error}")
if(NOT _negative_diagnostics MATCHES "unsupported semantic profile")
  message(FATAL_ERROR
    "Invalid-profile rejection omitted its diagnostic: "
    "${_negative_diagnostics}")
endif()

message(STATUS "Bioseq build-config CMake mapping contract passed")
