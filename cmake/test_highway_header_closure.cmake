cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED TEST_ROOT OR NOT IS_ABSOLUTE "${TEST_ROOT}")
  message(FATAL_ERROR
    "test_highway_header_closure.cmake requires absolute TEST_ROOT")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/BioseqHighwayCodegen.cmake")

set(_owned_target bioseq_highway_target_inventory_probe)

function(_contract_require_equal actual expected message_text)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "Highway header-closure contract failed: ${message_text}\n"
      "actual='${actual}'\nexpected='${expected}'")
  endif()
endfunction()

function(_contract_write path contents)
  get_filename_component(_directory "${path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_directory}")
  file(WRITE "${path}" "${contents}")
endfunction()

function(_contract_depfile_escape output value)
  set(_escaped "${value}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE " " "\\ " _escaped "${_escaped}")
  string(REPLACE "#" "\\#" _escaped "${_escaped}")
  string(REPLACE ":" "\\:" _escaped "${_escaped}")
  string(REPLACE "$" "$$" _escaped "${_escaped}")
  set(${output} "${_escaped}" PARENT_SCOPE)
endfunction()

function(_contract_write_depfile path target)
  set(_contents "${target}:")
  string(ASCII 92 _backslash)
  string(ASCII 10 _newline)
  foreach(_dependency IN LISTS ARGN)
    _contract_depfile_escape(_escaped "${_dependency}")
    string(APPEND _contents
      " ${_backslash}${_newline}  ${_escaped}")
  endforeach()
  string(APPEND _contents "\n")
  _contract_write("${path}" "${_contents}")
endfunction()

function(_contract_populate_fixture root_a root_b output_dependencies)
  set(_paths
    "${root_a}/hwy/highway.h"
    "${root_a}/hwy/detect_targets.h"
    "${root_a}/hwy/targets.h"
    "${root_a}/hwy/base.h"
    "${root_a}/hwy/internal/feature map.h"
    "${root_a}/hwy/internal/hash#guard.h"
    "${root_a}/hwy/internal/cost$record.h"
    "${root_b}/hwy/secondary.h")
  set(_ordinal 0)
  foreach(_path IN LISTS _paths)
    file(RELATIVE_PATH _relative "${root_a}" "${_path}")
    if(_relative MATCHES "^\.\./")
      file(RELATIVE_PATH _relative "${root_b}" "${_path}")
    endif()
    _contract_write("${_path}" "fixture-${_ordinal}-${_relative}\n")
    math(EXPR _ordinal "${_ordinal} + 1")
  endforeach()
  set(${output_dependencies} "${_paths}" PARENT_SCOPE)
endfunction()

function(_contract_expected_records output root_a root_b)
  set(_records)
  foreach(_relative IN ITEMS
      hwy/highway.h
      hwy/detect_targets.h
      hwy/targets.h
      hwy/base.h
      "hwy/internal/feature map.h"
      "hwy/internal/hash#guard.h"
      "hwy/internal/cost$record.h")
    file(SHA256 "${root_a}/${_relative}" _digest)
    list(APPEND _records "0/${_relative}\t${_digest}")
  endforeach()
  file(SHA256 "${root_b}/hwy/secondary.h" _secondary_digest)
  list(APPEND _records "1/hwy/secondary.h\t${_secondary_digest}")
  list(SORT _records)
  set(${output} "${_records}" PARENT_SCOPE)
endfunction()

function(_contract_run_negative case_name expected_diagnostic)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${TEST_ROOT}/negative-${case_name}"
      "-DBIOSEQ_HIGHWAY_NEGATIVE_CASE=${case_name}"
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  if(_status EQUAL 0)
    message(FATAL_ERROR
      "Highway header-closure negative contract '${case_name}' "
      "was accepted")
  endif()
  string(CONCAT _diagnostics "${_output}" "${_error}")
  if(NOT _diagnostics MATCHES "${expected_diagnostic}")
    message(FATAL_ERROR
      "Highway header-closure negative contract '${case_name}' failed "
      "with the wrong diagnostic:\n${_diagnostics}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${TEST_ROOT}")
set(_base "${TEST_ROOT}/base")
set(_root_a "${TEST_ROOT}/relocation a/include")
set(_root_b "${TEST_ROOT}/secondary/include")
set(_probe "${_base}/target_inventory_probe.cppm")
set(_system_header "${_base}/compiler/system_header.h")
set(_depfile "${_base}/target_inventory_probe.d")
_contract_write("${_probe}" "#include <hwy/highway.h>\n")
_contract_write("${_system_header}" "system-header\n")
_contract_populate_fixture("${_root_a}" "${_root_b}"
  _highway_dependencies)

if(DEFINED BIOSEQ_HIGHWAY_NEGATIVE_CASE)
  if(BIOSEQ_HIGHWAY_NEGATIVE_CASE STREQUAL "missing_depfile")
    _bioseq_highway_parse_depfile(
      _ignored "${_depfile}" "${_owned_target}" "${_base}")
  elseif(BIOSEQ_HIGHWAY_NEGATIVE_CASE STREQUAL "foreign_target")
    _contract_write_depfile(
      "${_depfile}" foreign_target ${_highway_dependencies})
    _bioseq_highway_parse_depfile(
      _ignored "${_depfile}" "${_owned_target}" "${_base}")
  elseif(BIOSEQ_HIGHWAY_NEGATIVE_CASE STREQUAL "multiple_rules")
    _contract_write_depfile(
      "${_depfile}" "${_owned_target}" ${_highway_dependencies})
    file(APPEND "${_depfile}" "foreign: foreign.h\n")
    _bioseq_highway_parse_depfile(
      _ignored "${_depfile}" "${_owned_target}" "${_base}")
  elseif(BIOSEQ_HIGHWAY_NEGATIVE_CASE STREQUAL "missing_dependency")
    _contract_write_depfile(
      "${_depfile}" "${_owned_target}"
      "${_root_a}/hwy/highway.h"
      "${_root_a}/hwy/detect_targets.h"
      "${_root_a}/hwy/targets.h"
      "${_root_a}/hwy/absent.h")
    _bioseq_highway_parse_depfile(
      _dependencies "${_depfile}" "${_owned_target}" "${_base}")
    _bioseq_highway_header_closure(
      _ignored_paths _ignored_records _ignored_digest
      INCLUDE_DIRS "${_root_a}"
      DEPENDENCIES ${_dependencies})
  elseif(BIOSEQ_HIGHWAY_NEGATIVE_CASE STREQUAL "missing_anchor")
    _bioseq_highway_header_closure(
      _ignored_paths _ignored_records _ignored_digest
      INCLUDE_DIRS "${_root_a}"
      DEPENDENCIES
        "${_root_a}/hwy/highway.h"
        "${_root_a}/hwy/base.h")
  else()
    message(FATAL_ERROR
      "unknown BIOSEQ_HIGHWAY_NEGATIVE_CASE="
      "${BIOSEQ_HIGHWAY_NEGATIVE_CASE}")
  endif()
  message(FATAL_ERROR
    "negative contract unexpectedly returned: "
    "${BIOSEQ_HIGHWAY_NEGATIVE_CASE}")
endif()

set(_depfile_dependencies
  "${_probe}"
  ${_highway_dependencies}
  "${_system_header}"
  "${_root_a}/hwy/base.h")
_contract_write_depfile(
  "${_depfile}" "${_owned_target}" ${_depfile_dependencies})
_bioseq_highway_parse_depfile(
  _parsed_dependencies "${_depfile}" "${_owned_target}" "${_base}")
_bioseq_highway_header_closure(
  _closure_paths _closure_records _closure_digest
  INCLUDE_DIRS "${_root_a}" "${_root_b}"
  DEPENDENCIES ${_parsed_dependencies})

_contract_expected_records(
  _expected_records "${_root_a}" "${_root_b}")
_contract_require_equal(
  "${_closure_records}" "${_expected_records}"
  "compiler dependency closure is not exact")
set(_expected_stream "bioseq-highway-header-closure-v1\n")
foreach(_record IN LISTS _expected_records)
  string(APPEND _expected_stream "${_record}\n")
endforeach()
string(SHA256 _expected_digest "${_expected_stream}")
_contract_require_equal(
  "${_closure_digest}" "${_expected_digest}"
  "closure digest does not seal the versioned record stream")

list(FIND _closure_paths "${_probe}" _probe_index)
if(NOT _probe_index EQUAL -1)
  message(FATAL_ERROR
    "Highway header-closure contract retained the generated probe source")
endif()
list(FIND _closure_paths "${_system_header}" _system_index)
if(NOT _system_index EQUAL -1)
  message(FATAL_ERROR
    "Highway header-closure contract retained a compiler system header")
endif()
get_property(_configure_dependencies DIRECTORY
  PROPERTY CMAKE_CONFIGURE_DEPENDS)
foreach(_closure_path IN LISTS _closure_paths)
  list(FIND _configure_dependencies "${_closure_path}" _configured_index)
  if(_configured_index EQUAL -1)
    message(FATAL_ERROR
      "Highway header-closure contract did not register ${_closure_path}")
  endif()
endforeach()

set(_relocated_root_a "${TEST_ROOT}/relocation-b/include")
set(_relocated_root_b "${TEST_ROOT}/relocation-b-secondary/include")
_contract_populate_fixture(
  "${_relocated_root_a}" "${_relocated_root_b}"
  _relocated_dependencies)
_bioseq_highway_header_closure(
  _relocated_paths _relocated_records _relocated_digest
  INCLUDE_DIRS "${_relocated_root_a}" "${_relocated_root_b}"
  DEPENDENCIES ${_relocated_dependencies})
_contract_require_equal(
  "${_relocated_records}" "${_closure_records}"
  "relocation changed root-relative closure records")
_contract_require_equal(
  "${_relocated_digest}" "${_closure_digest}"
  "relocation changed the Highway closure digest")

_contract_write(
  "${_relocated_root_a}/hwy/internal/hash#guard.h"
  "mutated-transitive-header\n")
_bioseq_highway_header_closure(
  _mutated_paths _mutated_records _mutated_digest
  INCLUDE_DIRS "${_relocated_root_a}" "${_relocated_root_b}"
  DEPENDENCIES ${_relocated_dependencies})
if(_mutated_digest STREQUAL _closure_digest)
  message(FATAL_ERROR
    "Highway header-closure contract did not detect transitive mutation")
endif()

_contract_run_negative(
  missing_depfile "target probe did not produce")
_contract_run_negative(
  foreign_target "foreign or ambiguous")
_contract_run_negative(
  multiple_rules "multiple logical")
_contract_run_negative(
  missing_dependency "target-probe dependency is not")
_contract_run_negative(
  missing_anchor "omits required")

message(STATUS "Highway header-closure script contracts passed")
