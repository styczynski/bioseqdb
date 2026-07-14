function(bioseq_parse_annotation_handler handler annotation_out output_kind_out)
  string(REPLACE "|" ";" _fields "${handler}")
  list(LENGTH _fields _field_count)
  if(NOT _field_count EQUAL 2)
    message(FATAL_ERROR
      "Invalid external annotation handler '${handler}'; expected "
      "'<annotation>|<output-kind>'")
  endif()
  list(GET _fields 0 _annotation)
  list(GET _fields 1 _output_kind)
  if(NOT _annotation MATCHES "^[A-Za-z_][A-Za-z0-9_:]*$")
    message(FATAL_ERROR
      "Invalid external annotation name '${_annotation}'")
  endif()
  if(NOT _output_kind MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
    message(FATAL_ERROR
      "Invalid external annotation output kind '${_output_kind}'")
  endif()
  set(${annotation_out} "${_annotation}" PARENT_SCOPE)
  set(${output_kind_out} "${_output_kind}" PARENT_SCOPE)
endfunction()

function(bioseq_find_registered_annotation source handlers_variable handler_out)
  set(_matched_handler)
  foreach(_handler IN LISTS ${handlers_variable})
    bioseq_parse_annotation_handler(
      "${_handler}" _annotation _output_kind)
    file(STRINGS "${source}" _marker
      REGEX "\\[\\[[ \t]*${_annotation}[ \t]*\\("
      LIMIT_COUNT 1)
    if(_marker)
      set(_matched_handler "${_handler}")
      break()
    endif()
  endforeach()
  set(${handler_out} "${_matched_handler}" PARENT_SCOPE)
endfunction()
