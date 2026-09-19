cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS
    SOURCE_LIST_FILE OUTPUT_FILE STAMP_FILE DEPFILE ANNOTATION_LITERAL)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR
      "scan_sql_annotation_owners.cmake requires ${_required}")
  endif()
endforeach()

foreach(_path_variable IN ITEMS
    SOURCE_LIST_FILE OUTPUT_FILE STAMP_FILE DEPFILE)
  set(_path "${${_path_variable}}")
  if(NOT IS_ABSOLUTE "${_path}")
    message(FATAL_ERROR
      "scan_sql_annotation_owners.cmake requires an absolute "
      "${_path_variable}: ${_path}")
  endif()
  cmake_path(NORMAL_PATH _path OUTPUT_VARIABLE _normalized_path)
  set(${_path_variable} "${_normalized_path}")
endforeach()

if(NOT EXISTS "${SOURCE_LIST_FILE}")
  message(FATAL_ERROR
    "SQL annotation-owner source list does not exist: ${SOURCE_LIST_FILE}")
endif()

foreach(_output_path IN ITEMS
    "${OUTPUT_FILE}" "${STAMP_FILE}" "${DEPFILE}")
  get_filename_component(_output_directory "${_output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_output_directory}")
endforeach()

file(STRINGS "${SOURCE_LIST_FILE}" _sources ENCODING UTF-8)
list(SORT _sources)
list(REMOVE_DUPLICATES _sources)

set(_annotated_sources)
set(_dependencies
  "${CMAKE_CURRENT_LIST_FILE}"
  "${SOURCE_LIST_FILE}")
foreach(_source IN LISTS _sources)
  if(_source STREQUAL "")
    continue()
  endif()
  if(NOT IS_ABSOLUTE "${_source}")
    message(FATAL_ERROR
      "SQL annotation-owner source is not absolute: ${_source}")
  endif()
  cmake_path(NORMAL_PATH _source OUTPUT_VARIABLE _source_normal)

  # GLOB_RECURSE CONFIGURE_DEPENDS owns filename topology. A file can vanish
  # before CMake's glob verification runs, so do not turn that transition into
  # a scanner failure; the configure pass will publish the new source list.
  if(NOT EXISTS "${_source_normal}" OR IS_DIRECTORY "${_source_normal}")
    continue()
  endif()

  list(APPEND _dependencies "${_source_normal}")
  file(READ "${_source_normal}" _source_content)
  string(FIND "${_source_content}" "${ANNOTATION_LITERAL}" _annotation_index)
  if(NOT _annotation_index EQUAL -1)
    file(REAL_PATH "${_source_normal}" _source_real)
    list(APPEND _annotated_sources "${_source_real}")
  endif()
endforeach()
list(SORT _annotated_sources)
list(REMOVE_DUPLICATES _annotated_sources)

string(JOIN "\n" _inventory ${_annotated_sources})
string(APPEND _inventory "\n")
set(_temporary "${OUTPUT_FILE}.tmp")
file(WRITE "${_temporary}" "${_inventory}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${_temporary}" "${OUTPUT_FILE}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${_temporary}")

function(_bioseq_sql_escape_depfile_path output_variable input_path)
  if("${input_path}" MATCHES "[\r\n]")
    message(FATAL_ERROR
      "SQL annotation-owner depfile path contains a newline")
  endif()
  set(_escaped "${input_path}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE " " "\\ " _escaped "${_escaped}")
  string(REPLACE "#" "\\#" _escaped "${_escaped}")
  string(REPLACE ":" "\\:" _escaped "${_escaped}")
  string(REPLACE "$" "$$" _escaped "${_escaped}")
  set(${output_variable} "${_escaped}" PARENT_SCOPE)
endfunction()

list(SORT _dependencies)
list(REMOVE_DUPLICATES _dependencies)
_bioseq_sql_escape_depfile_path(_escaped_stamp "${STAMP_FILE}")
set(_depfile_content "${_escaped_stamp}:")
foreach(_dependency IN LISTS _dependencies)
  _bioseq_sql_escape_depfile_path(
    _escaped_dependency "${_dependency}")
  string(APPEND _depfile_content " ${_escaped_dependency}")
endforeach()
string(APPEND _depfile_content "\n")
file(WRITE "${DEPFILE}" "${_depfile_content}")
file(TOUCH "${STAMP_FILE}")
