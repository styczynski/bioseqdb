include_guard(GLOBAL)

# Collect a deterministic authored-source inventory without admitting generated
# outputs or silently changing target ownership. Include patterns and exclusion
# paths are relative to BASE_DIRECTORY. Every exclusion must match exactly one
# collected source, and EXPECTED_COUNT makes additions or removals a reviewed
# configure-time change.
function(bioseq_collect_authored_sources output_variable)
  set(_bioseq_inventory_options RECURSE)
  set(_bioseq_inventory_one_value BASE_DIRECTORY EXPECTED_COUNT)
  set(_bioseq_inventory_multi_value INCLUDE_PATTERNS EXCLUDE_PATHS)
  cmake_parse_arguments(
    PARSE_ARGV 1
    _bioseq_inventory
    "${_bioseq_inventory_options}"
    "${_bioseq_inventory_one_value}"
    "${_bioseq_inventory_multi_value}")

  if(NOT output_variable MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR
      "bioseq_collect_authored_sources requires a valid output variable")
  endif()
  if(_bioseq_inventory_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "bioseq_collect_authored_sources received unknown arguments: "
      "${_bioseq_inventory_UNPARSED_ARGUMENTS}")
  endif()
  if(_bioseq_inventory_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR
      "bioseq_collect_authored_sources has missing values for: "
      "${_bioseq_inventory_KEYWORDS_MISSING_VALUES}")
  endif()
  if(NOT _bioseq_inventory_BASE_DIRECTORY)
    message(FATAL_ERROR
      "bioseq_collect_authored_sources requires BASE_DIRECTORY")
  endif()
  if(NOT _bioseq_inventory_INCLUDE_PATTERNS)
    message(FATAL_ERROR
      "bioseq_collect_authored_sources requires INCLUDE_PATTERNS")
  endif()
  if(NOT _bioseq_inventory_EXPECTED_COUNT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
      "bioseq_collect_authored_sources requires a positive EXPECTED_COUNT")
  endif()

  set(_bioseq_inventory_base "${_bioseq_inventory_BASE_DIRECTORY}")
  cmake_path(ABSOLUTE_PATH _bioseq_inventory_base NORMALIZE)
  if(NOT IS_DIRECTORY "${_bioseq_inventory_base}")
    message(FATAL_ERROR
      "bioseq_collect_authored_sources BASE_DIRECTORY is not a directory: "
      "${_bioseq_inventory_base}")
  endif()

  set(_bioseq_inventory_globs)
  foreach(_bioseq_inventory_pattern IN LISTS
      _bioseq_inventory_INCLUDE_PATTERNS)
    if(IS_ABSOLUTE "${_bioseq_inventory_pattern}"
        OR _bioseq_inventory_pattern MATCHES "(^|/)\\.\\.(/|$)"
        OR _bioseq_inventory_pattern MATCHES "\\$<")
      message(FATAL_ERROR
        "bioseq_collect_authored_sources requires a relative source-only "
        "include pattern: ${_bioseq_inventory_pattern}")
    endif()
    list(APPEND _bioseq_inventory_globs
      "${_bioseq_inventory_base}/${_bioseq_inventory_pattern}")
  endforeach()

  if(_bioseq_inventory_RECURSE)
    file(GLOB_RECURSE _bioseq_inventory_sources
      CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
      ${_bioseq_inventory_globs})
  else()
    file(GLOB _bioseq_inventory_sources
      CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
      ${_bioseq_inventory_globs})
  endif()
  list(REMOVE_DUPLICATES _bioseq_inventory_sources)
  list(SORT _bioseq_inventory_sources)

  foreach(_bioseq_inventory_exclusion IN LISTS
      _bioseq_inventory_EXCLUDE_PATHS)
    if(IS_ABSOLUTE "${_bioseq_inventory_exclusion}"
        OR _bioseq_inventory_exclusion MATCHES "(^|/)\\.\\.(/|$)"
        OR _bioseq_inventory_exclusion MATCHES "\\$<")
      message(FATAL_ERROR
        "bioseq_collect_authored_sources requires a relative source-only "
        "exclusion: ${_bioseq_inventory_exclusion}")
    endif()
    set(_bioseq_inventory_excluded_source
      "${_bioseq_inventory_base}/${_bioseq_inventory_exclusion}")
    cmake_path(NORMAL_PATH _bioseq_inventory_excluded_source)
    list(FIND _bioseq_inventory_sources
      "${_bioseq_inventory_excluded_source}"
      _bioseq_inventory_excluded_index)
    if(_bioseq_inventory_excluded_index EQUAL -1)
      message(FATAL_ERROR
        "bioseq_collect_authored_sources exclusion did not match an included "
        "source: ${_bioseq_inventory_exclusion}")
    endif()
    list(REMOVE_ITEM _bioseq_inventory_sources
      "${_bioseq_inventory_excluded_source}")
  endforeach()

  list(LENGTH _bioseq_inventory_sources _bioseq_inventory_source_count)
  if(NOT _bioseq_inventory_source_count EQUAL
      _bioseq_inventory_EXPECTED_COUNT)
    message(FATAL_ERROR
      "bioseq_collect_authored_sources expected "
      "${_bioseq_inventory_EXPECTED_COUNT} sources under "
      "${_bioseq_inventory_base}, got ${_bioseq_inventory_source_count}")
  endif()
  set(${output_variable} "${_bioseq_inventory_sources}" PARENT_SCOPE)
endfunction()
