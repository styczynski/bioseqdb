cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED test_root
   OR NOT IS_ABSOLUTE "${test_root}")
  message(FATAL_ERROR
    "private_postgres_artifact_writer_test.cmake requires absolute test_root")
endif()

set(_writer
  "${CMAKE_CURRENT_LIST_DIR}/../../write_private_postgres_artifacts.cmake")
set(_producer
  "${CMAKE_CURRENT_LIST_DIR}/../../bioseq_private_postgres_stage.cmake")
foreach(_contract_file IN ITEMS _writer _producer)
  if(NOT EXISTS "${${_contract_file}}"
     OR IS_DIRECTORY "${${_contract_file}}")
    message(FATAL_ERROR
      "private PostgreSQL artifact contract file is missing: "
      "${${_contract_file}}")
  endif()
endforeach()

function(_contract_assert_equal actual expected label)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "private PostgreSQL artifact ${label} mismatch\n"
      "actual='${actual}'\nexpected='${expected}'")
  endif()
endfunction()

function(_contract_assert_keys document)
  set(_expected_keys ${ARGN})
  list(SORT _expected_keys)
  string(JSON _key_count LENGTH "${document}")
  math(EXPR _last_key_index "${_key_count} - 1")
  set(_actual_keys)
  foreach(_key_index RANGE 0 ${_last_key_index})
    string(JSON _key MEMBER "${document}" "${_key_index}")
    list(APPEND _actual_keys "${_key}")
  endforeach()
  list(SORT _actual_keys)
  if(NOT "${_actual_keys}" STREQUAL "${_expected_keys}")
    message(FATAL_ERROR
      "private PostgreSQL artifact object is not closed\n"
      "actual='${_actual_keys}'\nexpected='${_expected_keys}'")
  endif()
endfunction()

function(_contract_write_input artifact_index suffix value)
  file(WRITE
    "${_input_root}/${artifact_index}-${suffix}"
    "${value}")
endfunction()

if(IS_SYMLINK "${test_root}")
  message(FATAL_ERROR "test_root must not be a symlink: ${test_root}")
endif()
file(MAKE_DIRECTORY "${test_root}")
if(NOT IS_DIRECTORY "${test_root}" OR IS_SYMLINK "${test_root}")
  message(FATAL_ERROR "test_root must be a real directory: ${test_root}")
endif()
set(_fixture_root "${test_root}/writer-fixture")
file(REMOVE_RECURSE "${_fixture_root}")
set(_input_root "${_fixture_root}/artifact-inputs")
set(_artifact_spec "${_fixture_root}/extension-artifacts.json")
file(MAKE_DIRECTORY "${_input_root}")

string(ASCII 9 _tab)
string(ASCII 10 _newline)
string(ASCII 34 _quote)
string(ASCII 92 _backslash)
string(CONCAT _control_path
  "${test_root}/control path/"
  "quoted${_quote}name${_quote}"
  "${_backslash}segment;tab${_tab}line${_newline}bioseq_cache.control")
string(CONCAT _install_sql_path
  "${test_root}/install sql/"
  "bioseq_cache;quoted${_quote}${_backslash}install.sql")
string(CONCAT _library_path
  "C:${_backslash}Program Files${_backslash}bioseq"
  "${_backslash}bioseq_cache.dll")

_contract_write_input(0 name.txt "bioseq_cache")
_contract_write_input(0 control.path "${_control_path}")
_contract_write_input(0 install_sql.path "${_install_sql_path}")
_contract_write_input(0 library.path "${_library_path}")
_contract_write_input(0 module_pathname_override.txt "bioseq_cache")
_contract_write_input(1 name.txt "bioseq")
_contract_write_input(1 control.path "${test_root}/bioseq.control")
_contract_write_input(1 install_sql.path "${test_root}/bioseq--1.0.sql")
_contract_write_input(1 library.path "${test_root}/libbioseq.so")
_contract_write_input(1 module_pathname_override.txt "-")
file(WRITE "${_artifact_spec}" "stale output\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-Dartifact_input_root=${_input_root}"
    "-Dartifact_count=2"
    "-Dartifact_spec_output=${_artifact_spec}"
    -P "${_writer}"
  RESULT_VARIABLE _writer_status
  OUTPUT_VARIABLE _writer_output
  ERROR_VARIABLE _writer_error)
if(NOT _writer_status EQUAL 0)
  message(FATAL_ERROR
    "private PostgreSQL artifact writer failed\n"
    "stdout:\n${_writer_output}\n"
    "stderr:\n${_writer_error}")
endif()
if(NOT EXISTS "${_artifact_spec}"
   OR IS_DIRECTORY "${_artifact_spec}"
   OR IS_SYMLINK "${_artifact_spec}")
  message(FATAL_ERROR
    "private PostgreSQL artifact writer did not produce a real file")
endif()

file(READ "${_artifact_spec}" _document)
_contract_assert_keys(
  "${_document}"
  artifacts
  schema
  schema_revision)
string(JSON _schema_type TYPE "${_document}" schema)
string(JSON _schema GET "${_document}" schema)
string(JSON _revision_type TYPE "${_document}" schema_revision)
string(JSON _revision GET "${_document}" schema_revision)
string(JSON _artifacts_type TYPE "${_document}" artifacts)
string(JSON _artifact_count LENGTH "${_document}" artifacts)
_contract_assert_equal("${_schema_type}" STRING "schema type")
_contract_assert_equal(
  "${_schema}" "bioseq.private_postgres.artifacts" "schema")
_contract_assert_equal("${_revision_type}" NUMBER "schema revision type")
_contract_assert_equal("${_revision}" 1 "schema revision")
_contract_assert_equal("${_artifacts_type}" ARRAY "artifacts type")
_contract_assert_equal("${_artifact_count}" 2 "artifact count")

string(JSON _cache_artifact GET "${_document}" artifacts 0)
_contract_assert_keys(
  "${_cache_artifact}"
  control
  install_sql
  library
  module_pathname_override
  name)
foreach(_cache_field IN ITEMS
    name
    control
    install_sql
    library
    module_pathname_override)
  string(JSON _field_type TYPE "${_cache_artifact}" "${_cache_field}")
  _contract_assert_equal(
    "${_field_type}" STRING "cache ${_cache_field} type")
endforeach()
string(JSON _actual_name GET "${_cache_artifact}" name)
string(JSON _actual_control GET "${_cache_artifact}" control)
string(JSON _actual_install_sql GET "${_cache_artifact}" install_sql)
string(JSON _actual_library GET "${_cache_artifact}" library)
string(JSON _actual_override GET
  "${_cache_artifact}" module_pathname_override)
_contract_assert_equal("${_actual_name}" bioseq_cache "cache name")
_contract_assert_equal(
  "${_actual_control}" "${_control_path}" "escaped control path")
_contract_assert_equal(
  "${_actual_install_sql}" "${_install_sql_path}" "escaped install SQL path")
_contract_assert_equal(
  "${_actual_library}" "${_library_path}" "escaped library path")
_contract_assert_equal(
  "${_actual_override}" bioseq_cache "module pathname override")

string(JSON _bioseq_artifact GET "${_document}" artifacts 1)
_contract_assert_keys(
  "${_bioseq_artifact}"
  control
  install_sql
  library
  name)

file(READ "${_producer}" _producer_source)
foreach(_required_source_text IN ITEMS
    "write_private_postgres_artifacts.cmake"
    "artifact-inputs"
    "-control.path"
    "-install_sql.path"
    "-library.path"
    "-Dartifact_input_root="
    "-Dartifact_count="
    "-Dartifact_spec_output=")
  string(FIND
    "${_producer_source}"
    "${_required_source_text}"
    _required_source_offset)
  if(_required_source_offset EQUAL -1)
    message(FATAL_ERROR
      "private PostgreSQL stage producer lost raw writer integration: "
      "${_required_source_text}")
  endif()
endforeach()
string(FIND "${_producer_source}" "_artifact_json" _inline_json_offset)
if(NOT _inline_json_offset EQUAL -1)
  message(FATAL_ERROR
    "private PostgreSQL stage producer still interpolates inline JSON")
endif()

message(STATUS "Private PostgreSQL artifact writer contract passed")
