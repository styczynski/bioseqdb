# Generates a Doxygen config fragment that sets INPUT to exactly the reachable
# interface-unit closure (§11.1). The website source_graph stage writes CLOSURE as a
# newline-delimited list of repository-relative source paths; this script joins them
# into a Doxygen `INPUT = a \<nl> b ...` fragment that the main Doxyfile @INCLUDEs, so
# Doxygen parses only the closure and never a recursive main/bioseq directory glob.
#
# Required -D arguments:
#   CLOSURE      reachable interface-unit list (one path per line)
#   SOURCE_ROOT  repository root the paths are relative to
#   FRAGMENT_OUT Doxygen config fragment to write

if(NOT EXISTS "${CLOSURE}")
  message(FATAL_ERROR "doxygen_input_fragment: closure file not found: ${CLOSURE}")
endif()
file(STRINGS "${CLOSURE}" _units)
set(_fragment "# Generated from the reachable interface-unit closure (§11.1).\nINPUT =")
foreach(_unit ${_units})
  if(_unit STREQUAL "")
    continue()
  endif()
  if(IS_ABSOLUTE "${_unit}")
    set(_abs "${_unit}")
  else()
    set(_abs "${SOURCE_ROOT}/${_unit}")
  endif()
  string(APPEND _fragment " \\\n    ${_abs}")
endforeach()
string(APPEND _fragment "\n")
file(WRITE "${FRAGMENT_OUT}" "${_fragment}")
