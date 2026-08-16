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

function(_require_occurrence_count text needle expected context)
  if("${needle}" STREQUAL "")
    message(FATAL_ERROR "${context}: occurrence marker must not be empty")
  endif()
  string(LENGTH "${text}" _text_length)
  string(LENGTH "${needle}" _needle_length)
  string(REPLACE "${needle}" "" _without_needle "${text}")
  string(LENGTH "${_without_needle}" _without_needle_length)
  math(EXPR _occurrence_bytes
    "${_text_length} - ${_without_needle_length}")
  math(EXPR _occurrence_count
    "${_occurrence_bytes} / ${_needle_length}")
  _require_equal("${_occurrence_count}" "${expected}" "${context}")
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

set(_canonical_build_config_entries
  "bioseq_template_flatten_profile^BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE^unsigned int^profile_ordinal^0..5^0U"
  "bioseq_template_flatten_has_all_alphabets_uint64^BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64^bool^boolean^false|true^false"
  "bioseq_template_flatten_has_all_coordinates^BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES^bool^boolean^false|true^false"
  "bioseq_template_flatten_has_adaptive_lengths^BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS^bool^boolean^false|true^false"
  "bioseq_template_flatten_has_extended_kmer_coordinates^BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES^bool^boolean^false|true^false"
  "bioseq_template_flatten_has_full_specialization^BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION^bool^boolean^false|true^false"
  "bioseq_scheduler_typed_heavy_unit_count^BIOSEQ_BUILD_CONFIG_SCHEDULER_TYPED_HEAVY_UNIT_COUNT^unsigned int^count^7|14|56^7U"
  "bioseq_test_indexes_enabled^BIOSEQ_BUILD_CONFIG_TEST_INDEXES_ENABLED^bool^boolean^false|true^false"
  "bioseq_cache_testing_enabled^BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED^bool^boolean^false|true^false"
  "bioseq_external_api_clients_enabled^BIOSEQ_BUILD_CONFIG_EXTERNAL_API_CLIENTS_ENABLED^bool^boolean^false|true^false"
  "bioseq_vacuum_truncate_lock_wait_interval_milliseconds^BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS^int^milliseconds^1..2147483647^50"
  "bioseq_vacuum_truncate_lock_timeout_milliseconds^BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS^int^milliseconds^1..2147483647^4500")
list(LENGTH _canonical_build_config_entries _canonical_build_config_entry_count)
_require_equal("${_canonical_build_config_entry_count}" 12
  "canonical build-config inventory size")

string(REGEX REPLACE "[ \t\r\n]+" "" _authored_config_compact
  "${_authored_config_source}")
file(READ
  "${_repository_root}/cmake/bioseq_build_config_codegen.cmake"
  _build_config_generator_source)
_require_occurrence_count("${_build_config_generator_source}" "--override" 12
  "canonical build-config override count")
string(REGEX MATCHALL "BIOSEQ_BUILD_CONFIG_[A-Z0-9_]+"
  _build_config_generator_macro_uses "${_build_config_generator_source}")
list(LENGTH _build_config_generator_macro_uses
  _build_config_generator_macro_use_count)
_require_equal("${_build_config_generator_macro_use_count}" 12
  "canonical build-config generator macro count")

foreach(_canonical_build_config_entry IN LISTS _canonical_build_config_entries)
  string(REPLACE "^" ";" _canonical_build_config_fields
    "${_canonical_build_config_entry}")
  list(LENGTH _canonical_build_config_fields
    _canonical_build_config_field_count)
  _require_equal("${_canonical_build_config_field_count}" 6
    "canonical build-config inventory field count")
  list(GET _canonical_build_config_fields 0 _canonical_build_config_name)
  list(GET _canonical_build_config_fields 1 _canonical_build_config_macro)
  list(GET _canonical_build_config_fields 2 _canonical_build_config_type)
  list(GET _canonical_build_config_fields 3 _canonical_build_config_unit)
  list(GET _canonical_build_config_fields 4 _canonical_build_config_domain)
  list(GET _canonical_build_config_fields 5 _canonical_build_config_default)
  string(REGEX REPLACE "[ \t\r\n]+" ""
    _canonical_build_config_compact_type
    "${_canonical_build_config_type}")
  string(CONCAT _canonical_build_config_declaration
    "inlineconstexpr${_canonical_build_config_compact_type}"
    "${_canonical_build_config_name}"
    "[[bioseq::build_config(\"${_canonical_build_config_macro}\","
    "\"${_canonical_build_config_unit}\","
    "\"${_canonical_build_config_domain}\")]]="
    "${_canonical_build_config_default};")
  _require_occurrence_count("${_authored_config_compact}"
    "${_canonical_build_config_declaration}" 1
    "canonical declaration ${_canonical_build_config_name}")
  _require_occurrence_count("${_authored_config_source}"
    "${_canonical_build_config_macro}" 1
    "canonical authored macro ${_canonical_build_config_macro}")
  _require_occurrence_count("${_build_config_generator_source}"
    "${_canonical_build_config_macro}" 1
    "canonical generator override ${_canonical_build_config_macro}")
endforeach()

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
file(READ
  "${_repository_root}/bioseq/backend/pg/table/access_method_parts/maintenance.cppm"
  _vacuum_maintenance_source)
_require_occurrence_count("${_vacuum_maintenance_source}"
  "import bioseq.build_config;" 1
  "vacuum build-config module import")
_require_occurrence_count("${_vacuum_maintenance_source}"
  "bioseq::build_config::bioseq_vacuum_truncate_lock_wait_interval_milliseconds"
  1 "vacuum retry constexpr consumer")
_require_occurrence_count("${_vacuum_maintenance_source}"
  "bioseq::build_config::bioseq_vacuum_truncate_lock_timeout_milliseconds"
  1 "vacuum timeout constexpr consumer")
string(FIND "${_bioseq_cmake_source}"
  [=[set(_bioseq_build_config_authored_input
  "${CMAKE_CURRENT_LIST_DIR}/backend/common/bioseq_build_config.cppm")]=]
  _authored_input_declaration)
set(_authored_input_path "backend/common/bioseq_build_config.cppm")
_require_occurrence_count("${_bioseq_cmake_source}" "${_authored_input_path}" 1
  "canonical authored build-config path declaration count")
_require_occurrence_count("${_bioseq_cmake_source}"
  "bioseq_add_build_config_module(" 1
  "generated build-config provider registration count")
string(FIND "${_bioseq_cmake_source}"
  [=[list(REMOVE_ITEM SRC_MOD_FILES
  "${_bioseq_build_config_authored_input}")]=]
  _authored_source_exclusion)
string(FIND "${_bioseq_cmake_source}"
  "bioseq_add_build_config_module(" _generator_registration)
string(FIND "${_bioseq_cmake_source}"
  "file(GLOB_RECURSE BIOSEQ_NOPG_MODULES" _nopg_inventory)
string(FIND "${_bioseq_cmake_source}"
  [=[list(REMOVE_ITEM BIOSEQ_NOPG_MODULES
  "${_bioseq_build_config_authored_input}")]=]
  _authored_nopg_exclusion)
string(FIND "${_bioseq_cmake_source}"
  "Authored build-config generator input escaped into broad module"
  _authored_ownership_guard)
if(_authored_input_declaration LESS 0
    OR _authored_source_exclusion LESS 0
    OR _generator_registration LESS 0
    OR _nopg_inventory LESS 0
    OR _authored_nopg_exclusion LESS 0
    OR _authored_ownership_guard LESS 0
    OR _authored_input_declaration GREATER _authored_source_exclusion
    OR _authored_source_exclusion GREATER _generator_registration
    OR _generator_registration GREATER _nopg_inventory
    OR _nopg_inventory GREATER _authored_nopg_exclusion
    OR _authored_nopg_exclusion GREATER _authored_ownership_guard)
  message(FATAL_ERROR
    "Authored build config is not generator-only in both broad module inventories")
endif()
_require_contains("${_bioseq_cmake_source}"
  [=[bioseq_add_build_config_module(
  TARGET bioseq_build_config
  SOURCE "${_bioseq_build_config_authored_input}"]=]
  "canonical generated build-config provider")

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
