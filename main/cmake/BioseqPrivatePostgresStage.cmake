# Build-time staging for one or more PostgreSQL extension targets.
#
# A stage is immutable from the build graph's point of view: one generated
# artifact specification and one custom command own its complete contents.
# Callers consume paths through read-only properties on the custom target.
#
#   bioseq_add_private_postgres_stage(
#     TARGET my_private_postgres_stage
#     PG_CONFIG "${PostgreSQL_CONFIG}"       # optional when this is set
#     STAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/private-postgres" # optional
#     EXTENSION_NAMES bioseq_cache bioseq
#     EXTENSION_TARGETS postgresql_bioseq_cache postgresql_bioseq
#     CONTROL_PROPERTIES
#       BIOSEQ_EXTENSION_CONTROL_FILE
#       BIOSEQ_EXTENSION_CONTROL_FILE
#     INSTALL_SQL_PROPERTIES
#       BIOSEQ_EXTENSION_INSTALL_SQL
#       BIOSEQ_EXTENSION_INSTALL_SQL
#     MODULE_PATHNAME_OVERRIDES bioseq_cache bioseq) # optional
#
# MODULE_PATHNAME_OVERRIDES is either omitted or parallel to the other lists.
# Use "-" for an artifact which needs no override in a mixed vector. Overrides
# are bare library names, never "$libdir/..." paths or filenames with suffixes.

include_guard(GLOBAL)

function(bioseq_add_private_postgres_stage)
  set(_options)
  set(_one_value_args TARGET PG_CONFIG STAGE_ROOT)
  set(_multi_value_args
    EXTENSION_NAMES
    EXTENSION_TARGETS
    CONTROL_PROPERTIES
    INSTALL_SQL_PROPERTIES
    MODULE_PATHNAME_OVERRIDES)
  cmake_parse_arguments(
    PARSE_ARGV 0
    BIOSEQ_STAGE
    "${_options}"
    "${_one_value_args}"
    "${_multi_value_args}")

  if(BIOSEQ_STAGE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: unknown arguments: "
      "${BIOSEQ_STAGE_UNPARSED_ARGUMENTS}")
  endif()
  if(BIOSEQ_STAGE_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: missing values for: "
      "${BIOSEQ_STAGE_KEYWORDS_MISSING_VALUES}")
  endif()
  if(NOT BIOSEQ_STAGE_TARGET)
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: TARGET is required")
  endif()
  if(NOT BIOSEQ_STAGE_TARGET MATCHES "^[A-Za-z0-9_.+-]+$")
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: TARGET must be a path-safe "
      "CMake target name: ${BIOSEQ_STAGE_TARGET}")
  endif()
  if(TARGET "${BIOSEQ_STAGE_TARGET}")
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: target already exists: "
      "${BIOSEQ_STAGE_TARGET}")
  endif()

  list(LENGTH BIOSEQ_STAGE_EXTENSION_NAMES _extension_count)
  if(_extension_count EQUAL 0)
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: EXTENSION_NAMES must not be empty")
  endif()
  foreach(_parallel_list IN ITEMS
      BIOSEQ_STAGE_EXTENSION_TARGETS
      BIOSEQ_STAGE_CONTROL_PROPERTIES
      BIOSEQ_STAGE_INSTALL_SQL_PROPERTIES)
    list(LENGTH "${_parallel_list}" _parallel_count)
    if(NOT _parallel_count EQUAL _extension_count)
      message(FATAL_ERROR
        "bioseq_add_private_postgres_stage: ${_parallel_list} must contain "
        "exactly ${_extension_count} entries")
    endif()
  endforeach()

  list(LENGTH BIOSEQ_STAGE_MODULE_PATHNAME_OVERRIDES _override_count)
  if(_override_count EQUAL 0)
    foreach(_unused RANGE 1 ${_extension_count})
      list(APPEND BIOSEQ_STAGE_MODULE_PATHNAME_OVERRIDES "-")
    endforeach()
  elseif(NOT _override_count EQUAL _extension_count)
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: "
      "MODULE_PATHNAME_OVERRIDES must be omitted or contain exactly "
      "${_extension_count} entries (use '-' for no override)")
  endif()

  if(BIOSEQ_STAGE_PG_CONFIG)
    set(_pg_config "${BIOSEQ_STAGE_PG_CONFIG}")
  elseif(DEFINED PostgreSQL_CONFIG AND NOT "${PostgreSQL_CONFIG}" STREQUAL "")
    set(_pg_config "${PostgreSQL_CONFIG}")
  else()
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: PG_CONFIG is required when "
      "PostgreSQL_CONFIG is not set")
  endif()
  if(NOT EXISTS "${_pg_config}" OR IS_DIRECTORY "${_pg_config}")
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: pg_config is not a file: "
      "${_pg_config}")
  endif()

  execute_process(
    COMMAND "${_pg_config}" --version
    OUTPUT_VARIABLE _postgres_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _postgres_version_rc)
  if(NOT _postgres_version_rc EQUAL 0
     OR NOT _postgres_version MATCHES "^PostgreSQL 17([.]|[ \t]|$)")
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage requires PostgreSQL 17; "
      "${_pg_config} reported '${_postgres_version}'")
  endif()
  execute_process(
    COMMAND "${_pg_config}" --bindir
    OUTPUT_VARIABLE _postgres_bindir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _postgres_bindir_rc)
  if(NOT _postgres_bindir_rc EQUAL 0
     OR NOT IS_ABSOLUTE "${_postgres_bindir}")
    message(FATAL_ERROR
      "bioseq_add_private_postgres_stage: pg_config --bindir did not "
      "return an absolute path")
  endif()

  set(_safe_stage_parent
    "${CMAKE_CURRENT_BINARY_DIR}/private_postgres")
  cmake_path(
    ABSOLUTE_PATH _safe_stage_parent
    NORMALIZE
    OUTPUT_VARIABLE _safe_stage_parent)
  if(BIOSEQ_STAGE_STAGE_ROOT)
    cmake_path(
      ABSOLUTE_PATH BIOSEQ_STAGE_STAGE_ROOT
      BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      NORMALIZE
      OUTPUT_VARIABLE _stage_root_base)
    get_filename_component(
      _requested_stage_parent "${_stage_root_base}" DIRECTORY)
    if(NOT _requested_stage_parent STREQUAL _safe_stage_parent)
      message(FATAL_ERROR
        "bioseq_add_private_postgres_stage: STAGE_ROOT must be a direct "
        "child of ${_safe_stage_parent}: ${_stage_root_base}")
    endif()
  else()
    set(_stage_root_base
      "${_safe_stage_parent}/${BIOSEQ_STAGE_TARGET}")
  endif()
  if(CMAKE_CONFIGURATION_TYPES)
    set(_stage_parent "${_stage_root_base}")
    set(_stage_root "${_stage_root_base}/$<CONFIG>")
  else()
    set(_stage_root "${_stage_root_base}")
    set(_stage_parent "${_safe_stage_parent}")
  endif()
  file(MAKE_DIRECTORY "${_stage_parent}")

  string(CONCAT _metadata_root
    "${CMAKE_CURRENT_BINARY_DIR}/generated/private_postgres/"
    "${BIOSEQ_STAGE_TARGET}")
  file(MAKE_DIRECTORY "${_metadata_root}")
  if(CMAKE_CONFIGURATION_TYPES)
    set(_artifact_spec
      "${_metadata_root}/$<CONFIG>/extension-artifacts.json")
    set(_stage_manifest
      "${_metadata_root}/$<CONFIG>/stage-manifest.json")
    set(_stage_hash
      "${_metadata_root}/$<CONFIG>/staged-extensions.sha256")
  else()
    set(_artifact_spec "${_metadata_root}/extension-artifacts.json")
    set(_stage_manifest "${_metadata_root}/stage-manifest.json")
    set(_stage_hash "${_metadata_root}/staged-extensions.sha256")
  endif()

  string(CONCAT _artifact_json
    "{\n  \"schema\": \"bioseq_private_postgres_artifacts/v1\",\n"
    "  \"artifacts\": [\n")
  set(_artifact_dependencies)
  set(_extension_targets)
  set(_seen_extension_names)
  math(EXPR _last_extension_index "${_extension_count} - 1")
  foreach(_index RANGE 0 ${_last_extension_index})
    list(GET BIOSEQ_STAGE_EXTENSION_NAMES ${_index} _extension_name)
    list(GET BIOSEQ_STAGE_EXTENSION_TARGETS ${_index} _extension_target)
    list(GET BIOSEQ_STAGE_CONTROL_PROPERTIES ${_index} _control_property)
    list(GET BIOSEQ_STAGE_INSTALL_SQL_PROPERTIES ${_index} _install_property)
    list(GET
      BIOSEQ_STAGE_MODULE_PATHNAME_OVERRIDES
      ${_index}
      _module_pathname_override)

    if(NOT _extension_name MATCHES "^[a-z][a-z0-9_]*$")
      message(FATAL_ERROR
        "bioseq_add_private_postgres_stage: invalid extension name "
        "'${_extension_name}'")
    endif()
    if(_extension_name IN_LIST _seen_extension_names)
      message(FATAL_ERROR
        "bioseq_add_private_postgres_stage: duplicate extension name "
        "'${_extension_name}'")
    endif()
    list(APPEND _seen_extension_names "${_extension_name}")
    if(NOT TARGET "${_extension_target}")
      message(FATAL_ERROR
        "bioseq_add_private_postgres_stage: unknown extension target "
        "'${_extension_target}'")
    endif()
    foreach(_property IN ITEMS _control_property _install_property)
      if(NOT "${${_property}}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
          "bioseq_add_private_postgres_stage: invalid target property name "
          "'${${_property}}'")
      endif()
      get_target_property(
        _published_value "${_extension_target}" "${${_property}}")
      if(NOT _published_value
         OR _published_value MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
          "bioseq_add_private_postgres_stage: ${_extension_target} does not "
          "publish ${${_property}}")
      endif()
    endforeach()
    if(NOT _module_pathname_override STREQUAL "-"
       AND NOT _module_pathname_override MATCHES "^[a-z][a-z0-9_]*$")
      message(FATAL_ERROR
        "bioseq_add_private_postgres_stage: module pathname override must "
        "be a bare lower-case library name or '-': "
        "'${_module_pathname_override}'")
    endif()

    string(CONCAT _control_genex
      "$<TARGET_GENEX_EVAL:${_extension_target},"
      "$<TARGET_PROPERTY:${_extension_target},${_control_property}>>")
    string(CONCAT _install_genex
      "$<TARGET_GENEX_EVAL:${_extension_target},"
      "$<TARGET_PROPERTY:${_extension_target},${_install_property}>>")
    set(_library_genex "$<TARGET_FILE:${_extension_target}>")

    if(_index GREATER 0)
      string(APPEND _artifact_json ",\n")
    endif()
    string(APPEND _artifact_json
      "    {\n"
      "      \"name\": \"${_extension_name}\",\n"
      "      \"control\": \"${_control_genex}\",\n"
      "      \"install_sql\": \"${_install_genex}\",\n"
      "      \"library\": \"${_library_genex}\"")
    if(NOT _module_pathname_override STREQUAL "-")
      string(APPEND _artifact_json
        ",\n      \"module_pathname_override\": "
        "\"${_module_pathname_override}\"")
    endif()
    string(APPEND _artifact_json "\n    }")

    list(APPEND _extension_targets "${_extension_target}")
    list(APPEND _artifact_dependencies
      "${_control_genex}"
      "${_install_genex}")
  endforeach()
  string(APPEND _artifact_json "\n  ]\n}\n")

  file(GENERATE
    OUTPUT "${_artifact_spec}"
    CONTENT "${_artifact_json}")

  set(_stage_script
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/stage_private_postgres.cmake")
  add_custom_command(
    OUTPUT
      "${_stage_manifest}"
      "${_stage_hash}"
    COMMAND
      "${CMAKE_COMMAND}"
      "-DPG_CONFIG=${_pg_config}"
      "-DSTAGE_ROOT=${_stage_root}"
      "-DSTAGE_PARENT=${_stage_parent}"
      "-DARTIFACT_SPEC=${_artifact_spec}"
      "-DMANIFEST_OUT=${_stage_manifest}"
      "-DHASH_OUT=${_stage_hash}"
      -P "${_stage_script}"
    DEPENDS
      ${_extension_targets}
      ${_artifact_dependencies}
      "${_artifact_spec}"
      "${_stage_script}"
      "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    COMMENT
      "Staging immutable PostgreSQL 17 runtime for ${BIOSEQ_STAGE_TARGET}"
    VERBATIM)
  add_custom_target(
    "${BIOSEQ_STAGE_TARGET}"
    DEPENDS
      "${_stage_manifest}"
      "${_stage_hash}")

  string(REGEX REPLACE "^/+" "" _postgres_bindir_relative
    "${_postgres_bindir}")
  set(_staged_postgres_bin_dir
    "${_stage_root}/${_postgres_bindir_relative}")
  set_target_properties("${BIOSEQ_STAGE_TARGET}" PROPERTIES
    BIOSEQ_POSTGRES_BIN_DIR "${_staged_postgres_bin_dir}"
    BIOSEQ_POSTGRES_STAGE_ROOT "${_stage_root}"
    BIOSEQ_POSTGRES_STAGE_MANIFEST "${_stage_manifest}"
    BIOSEQ_POSTGRES_STAGE_HASH "${_stage_hash}"
    BIOSEQ_POSTGRES_ARTIFACT_SPEC "${_artifact_spec}"
    BIOSEQ_POSTGRES_EXTENSION_NAMES "${BIOSEQ_STAGE_EXTENSION_NAMES}")

  # Also expose the most recently declared stage to its immediate caller.
  # Multi-stage consumers should prefer the target property to avoid ambiguity.
  set(BIOSEQ_POSTGRES_BIN_DIR
    "${_staged_postgres_bin_dir}"
    PARENT_SCOPE)
endfunction()
