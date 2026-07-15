cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS
    SOURCE_ROOT OUTPUT_FILE STAMP_FILE DEPFILE SOURCE_LIST_FILE
    ANNOTATION_HANDLERS)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "scan_annotations.cmake requires ${_required}")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/annotation_registry.cmake")

file(GLOB_RECURSE _sources LIST_DIRECTORIES FALSE
  "${SOURCE_ROOT}/*.cppm")
list(SORT _sources)

set(_inventory "handlers=${ANNOTATION_HANDLERS}\n")
foreach(_source IN LISTS _sources)
  bioseq_find_registered_annotation(
    "${_source}" ANNOTATION_HANDLERS _handler)
  if(_handler)
    string(APPEND _inventory "descriptor=${_handler}\t${_source}\n")
  endif()
endforeach()

set(_temporary "${OUTPUT_FILE}.tmp")
file(WRITE "${_temporary}" "${_inventory}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${_temporary}" "${OUTPUT_FILE}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${_temporary}")

set(_dependencies
  "${SOURCE_LIST_FILE}"
  "${CMAKE_CURRENT_LIST_FILE}")
list(APPEND _dependencies ${_sources})
set(_depfile_content "${STAMP_FILE}:")
foreach(_dependency IN LISTS _dependencies)
  string(REPLACE "\\" "\\\\" _escaped "${_dependency}")
  string(REPLACE " " "\\ " _escaped "${_escaped}")
  string(REPLACE "#" "\\#" _escaped "${_escaped}")
  string(APPEND _depfile_content " ${_escaped}")
endforeach()
string(APPEND _depfile_content "\n")
file(WRITE "${DEPFILE}" "${_depfile_content}")
file(TOUCH "${STAMP_FILE}")
