cmake_minimum_required(VERSION 3.20)

foreach(_required_variable IN ITEMS
    artifact_input_root
    artifact_count
    artifact_spec_output)
  if(NOT DEFINED ${_required_variable}
     OR "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR
      "write_private_postgres_artifacts.cmake requires "
      "${_required_variable}")
  endif()
endforeach()

if(NOT IS_ABSOLUTE "${artifact_input_root}"
   OR NOT IS_DIRECTORY "${artifact_input_root}"
   OR IS_SYMLINK "${artifact_input_root}")
  message(FATAL_ERROR
    "artifact_input_root must be an absolute real directory: "
    "${artifact_input_root}")
endif()
if(NOT "${artifact_count}" MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
    "artifact_count must be a positive decimal integer: ${artifact_count}")
endif()
if(NOT IS_ABSOLUTE "${artifact_spec_output}")
  message(FATAL_ERROR
    "artifact_spec_output must be absolute: ${artifact_spec_output}")
endif()

function(_bioseq_private_postgres_read_input
    output_variable input_path input_label)
  if(NOT EXISTS "${input_path}"
     OR IS_DIRECTORY "${input_path}"
     OR IS_SYMLINK "${input_path}")
    message(FATAL_ERROR
      "${input_label} must be a real input file: ${input_path}")
  endif()
  file(READ "${input_path}" _input_value)
  set(${output_variable} "${_input_value}" PARENT_SCOPE)
endfunction()

function(_bioseq_private_postgres_json_string output_variable value)
  set(_escaped "${value}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")

  set(_control_hex_codes
    01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
    10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f)
  set(_control_code 1)
  foreach(_control_hex IN LISTS _control_hex_codes)
    string(ASCII "${_control_code}" _control_character)
    string(REPLACE
      "${_control_character}"
      "\\u00${_control_hex}"
      _escaped
      "${_escaped}")
    math(EXPR _control_code "${_control_code} + 1")
  endforeach()

  set(${output_variable} "\"${_escaped}\"" PARENT_SCOPE)
endfunction()

function(_bioseq_private_postgres_set_json_string
    output_variable document member value)
  _bioseq_private_postgres_json_string(_encoded_value "${value}")
  string(JSON _updated_document
    SET "${document}" "${member}" "${_encoded_value}")
  set(${output_variable} "${_updated_document}" PARENT_SCOPE)
endfunction()

set(_document "{}")
_bioseq_private_postgres_set_json_string(
  _document
  "${_document}"
  schema
  "bioseq.private_postgres.artifacts")
string(JSON _document SET "${_document}" schema_revision 1)
string(JSON _document SET "${_document}" artifacts "[]")

set(_seen_artifact_names)
math(EXPR _last_artifact_index "${artifact_count} - 1")
foreach(_artifact_index RANGE 0 ${_last_artifact_index})
  set(_input_prefix "${artifact_input_root}/${_artifact_index}")
  _bioseq_private_postgres_read_input(
    _artifact_name
    "${_input_prefix}-name.txt"
    "artifact ${_artifact_index} name")
  _bioseq_private_postgres_read_input(
    _control_path
    "${_input_prefix}-control.path"
    "artifact ${_artifact_index} control path")
  _bioseq_private_postgres_read_input(
    _install_sql_path
    "${_input_prefix}-install_sql.path"
    "artifact ${_artifact_index} install SQL path")
  _bioseq_private_postgres_read_input(
    _library_path
    "${_input_prefix}-library.path"
    "artifact ${_artifact_index} library path")
  _bioseq_private_postgres_read_input(
    _module_pathname_override
    "${_input_prefix}-module_pathname_override.txt"
    "artifact ${_artifact_index} module pathname override")

  if(NOT _artifact_name MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR
      "artifact ${_artifact_index} has an invalid name: ${_artifact_name}")
  endif()
  if(_artifact_name IN_LIST _seen_artifact_names)
    message(FATAL_ERROR "duplicate artifact name: ${_artifact_name}")
  endif()
  list(APPEND _seen_artifact_names "${_artifact_name}")
  foreach(_required_path IN ITEMS
      _control_path
      _install_sql_path
      _library_path)
    if("${${_required_path}}" STREQUAL "")
      message(FATAL_ERROR
        "artifact ${_artifact_name} has an empty ${_required_path}")
    endif()
  endforeach()
  if(NOT _module_pathname_override STREQUAL "-"
     AND NOT _module_pathname_override MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR
      "artifact ${_artifact_name} has an invalid module pathname override: "
      "${_module_pathname_override}")
  endif()

  set(_artifact "{}")
  _bioseq_private_postgres_set_json_string(
    _artifact "${_artifact}" name "${_artifact_name}")
  _bioseq_private_postgres_set_json_string(
    _artifact "${_artifact}" control "${_control_path}")
  _bioseq_private_postgres_set_json_string(
    _artifact "${_artifact}" install_sql "${_install_sql_path}")
  _bioseq_private_postgres_set_json_string(
    _artifact "${_artifact}" library "${_library_path}")
  if(NOT _module_pathname_override STREQUAL "-")
    _bioseq_private_postgres_set_json_string(
      _artifact
      "${_artifact}"
      module_pathname_override
      "${_module_pathname_override}")
  endif()
  string(JSON _document
    SET "${_document}" artifacts "${_artifact_index}" "${_artifact}")
endforeach()

get_filename_component(
  _artifact_spec_parent "${artifact_spec_output}" DIRECTORY)
file(MAKE_DIRECTORY "${_artifact_spec_parent}")
if(NOT IS_DIRECTORY "${_artifact_spec_parent}"
   OR IS_SYMLINK "${_artifact_spec_parent}")
  message(FATAL_ERROR
    "artifact specification parent must be a real directory: "
    "${_artifact_spec_parent}")
endif()
if(IS_DIRECTORY "${artifact_spec_output}"
   OR IS_SYMLINK "${artifact_spec_output}")
  message(FATAL_ERROR
    "artifact specification output must be absent or a real file: "
    "${artifact_spec_output}")
endif()

set(_temporary_output "${artifact_spec_output}.tmp")
if(IS_DIRECTORY "${_temporary_output}" OR IS_SYMLINK "${_temporary_output}")
  message(FATAL_ERROR
    "artifact specification temporary output must be absent or a real file: "
    "${_temporary_output}")
endif()
file(REMOVE "${_temporary_output}")
file(WRITE "${_temporary_output}" "${_document}\n")
file(RENAME "${_temporary_output}" "${artifact_spec_output}")
if(NOT EXISTS "${artifact_spec_output}"
   OR IS_DIRECTORY "${artifact_spec_output}"
   OR IS_SYMLINK "${artifact_spec_output}")
  message(FATAL_ERROR
    "artifact specification writer did not produce a real file: "
    "${artifact_spec_output}")
endif()
