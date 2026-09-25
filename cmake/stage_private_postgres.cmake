# Relocatable private PostgreSQL extension stage.
#
# Run with one generated artifact specification:
#
#   -DARTIFACT_SPEC=/absolute/path/extension-artifacts.json
#
# {
#   "schema": "bioseq.private_postgres.artifacts",
#   "schema_revision": 1,
#   "artifacts": [{
#     "name": "bioseq_cache",
#     "control": "/absolute/path/bioseq_cache.control",
#     "install_sql": "/absolute/path/bioseq_cache--1.0.sql",
#     "library": "/absolute/path/bioseq_cache.so",
#     "module_pathname_override": "bioseq_cache"
#   }]
# }
#
# It stages a relocatable, fully materialized copy of the selected PostgreSQL
# package-owned bindir/sharedir/pkglibdir under STAGE_ROOT. The pg_config
# coordinates remain stable, but no source symbolic link survives in the
# stage. It excludes façade-only global extensions, adds ONLY the current build
# targets' exact control/install-SQL/DSO sets, and writes stage-manifest.json.
# There is no fallback that installs globally or bootstraps a fixed schema. The
# artifact specification is the sole extension-artifact input.

cmake_minimum_required(VERSION 3.24)

foreach(_unsupported_single_artifact_arg IN ITEMS
    CONTROL_FILE INSTALL_SQL LIBRARY_FILE EXTENSION_VERSION)
  if(DEFINED ${_unsupported_single_artifact_arg})
    message(FATAL_ERROR
      "stage_private_postgres: unsupported single-artifact input "
      "-D${_unsupported_single_artifact_arg}; use -DARTIFACT_SPEC")
  endif()
endforeach()

foreach(_required
    PG_CONFIG STAGE_ROOT STAGE_PARENT ARTIFACT_SPEC MANIFEST_OUT HASH_OUT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "stage_private_postgres: -D${_required} is required")
  endif()
endforeach()

if(NOT EXISTS "${ARTIFACT_SPEC}"
   OR IS_DIRECTORY "${ARTIFACT_SPEC}"
   OR IS_SYMLINK "${ARTIFACT_SPEC}")
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC must be a real file: "
    "${ARTIFACT_SPEC}")
endif()

set(_artifact_names)
set(_artifact_controls)
set(_artifact_install_sqls)
set(_artifact_libraries)
set(_artifact_module_pathname_overrides)

file(READ "${ARTIFACT_SPEC}" _artifact_spec_json)
string(JSON _artifact_spec_root_type
  ERROR_VARIABLE _artifact_spec_root_error
  TYPE "${_artifact_spec_json}")
if(_artifact_spec_root_error
   OR NOT _artifact_spec_root_type STREQUAL "OBJECT")
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC root must be an object")
endif()

string(JSON _top_level_member_count LENGTH "${_artifact_spec_json}")
if(NOT _top_level_member_count EQUAL 3)
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC must contain exactly schema, "
    "schema_revision, and artifacts")
endif()
math(EXPR _top_level_last "${_top_level_member_count} - 1")
foreach(_member_index RANGE 0 ${_top_level_last})
  string(JSON _member_name
    MEMBER "${_artifact_spec_json}" ${_member_index})
  if(NOT _member_name STREQUAL "schema"
     AND NOT _member_name STREQUAL "schema_revision"
     AND NOT _member_name STREQUAL "artifacts")
    message(FATAL_ERROR
      "stage_private_postgres: unknown ARTIFACT_SPEC member "
      "'${_member_name}'")
  endif()
endforeach()

string(JSON _artifact_spec_schema_type
  ERROR_VARIABLE _artifact_spec_schema_type_error
  TYPE "${_artifact_spec_json}" schema)
string(JSON _artifact_spec_schema
  ERROR_VARIABLE _artifact_spec_schema_error
  GET "${_artifact_spec_json}" schema)
if(_artifact_spec_schema_type_error
   OR _artifact_spec_schema_error
   OR NOT _artifact_spec_schema_type STREQUAL "STRING"
   OR NOT _artifact_spec_schema STREQUAL
     "bioseq.private_postgres.artifacts")
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC schema must be "
    "bioseq.private_postgres.artifacts")
endif()

string(JSON _artifact_spec_revision_type
  ERROR_VARIABLE _artifact_spec_revision_type_error
  TYPE "${_artifact_spec_json}" schema_revision)
string(JSON _artifact_spec_revision
  ERROR_VARIABLE _artifact_spec_revision_error
  GET "${_artifact_spec_json}" schema_revision)
if(_artifact_spec_revision_type_error
   OR _artifact_spec_revision_error
   OR NOT _artifact_spec_revision_type STREQUAL "NUMBER"
   OR NOT _artifact_spec_revision STREQUAL "1")
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC schema_revision must be the "
    "numeric current revision 1")
endif()

string(JSON _artifact_spec_type
  ERROR_VARIABLE _artifact_spec_type_error
  TYPE "${_artifact_spec_json}" artifacts)
if(_artifact_spec_type_error OR NOT _artifact_spec_type STREQUAL "ARRAY")
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC artifacts must be an array")
endif()
string(JSON _artifact_count LENGTH "${_artifact_spec_json}" artifacts)
if(_artifact_count LESS 1)
  message(FATAL_ERROR
    "stage_private_postgres: ARTIFACT_SPEC artifacts must not be empty")
endif()

math(EXPR _last_artifact_index "${_artifact_count} - 1")
foreach(_artifact_index RANGE 0 ${_last_artifact_index})
  string(JSON _artifact_type
    TYPE "${_artifact_spec_json}" artifacts ${_artifact_index})
  if(NOT _artifact_type STREQUAL "OBJECT")
    message(FATAL_ERROR
      "stage_private_postgres: artifact ${_artifact_index} must be an "
      "object")
  endif()
  string(JSON _artifact_member_count
    LENGTH "${_artifact_spec_json}" artifacts ${_artifact_index})
  if(_artifact_member_count LESS 4 OR _artifact_member_count GREATER 5)
    message(FATAL_ERROR
      "stage_private_postgres: artifact ${_artifact_index} must contain "
      "exactly name, control, install_sql, library, and optional "
      "module_pathname_override")
  endif()
  math(EXPR _artifact_member_last "${_artifact_member_count} - 1")
  foreach(_member_index RANGE 0 ${_artifact_member_last})
    string(JSON _member_name
      MEMBER
      "${_artifact_spec_json}"
      artifacts
      ${_artifact_index}
      ${_member_index})
    if(NOT _member_name STREQUAL "name"
       AND NOT _member_name STREQUAL "control"
       AND NOT _member_name STREQUAL "install_sql"
       AND NOT _member_name STREQUAL "library"
       AND NOT _member_name STREQUAL "module_pathname_override")
      message(FATAL_ERROR
        "stage_private_postgres: unknown member '${_member_name}' in "
        "artifact ${_artifact_index}")
    endif()
  endforeach()

  foreach(_required_member IN ITEMS name control install_sql library)
    string(JSON _member_value
      ERROR_VARIABLE _member_error
      GET
      "${_artifact_spec_json}"
      artifacts
      ${_artifact_index}
      ${_required_member})
    if(_member_error OR "${_member_value}" STREQUAL "")
      message(FATAL_ERROR
        "stage_private_postgres: artifact ${_artifact_index} requires "
        "nonempty string member '${_required_member}'")
    endif()
    string(JSON _member_type
      TYPE
      "${_artifact_spec_json}"
      artifacts
      ${_artifact_index}
      ${_required_member})
    if(NOT _member_type STREQUAL "STRING")
      message(FATAL_ERROR
        "stage_private_postgres: artifact ${_artifact_index} member "
        "'${_required_member}' must be a string")
    endif()
    set("_artifact_${_artifact_index}_${_required_member}"
      "${_member_value}")
  endforeach()

  string(JSON _override_type
    ERROR_VARIABLE _override_error
    TYPE
    "${_artifact_spec_json}"
    artifacts
    ${_artifact_index}
    module_pathname_override)
  if(_override_error)
    set(_module_pathname_override "-")
  elseif(NOT _override_type STREQUAL "STRING")
    message(FATAL_ERROR
      "stage_private_postgres: artifact ${_artifact_index} "
      "module_pathname_override must be a string when present")
  else()
    string(JSON _module_pathname_override
      GET
      "${_artifact_spec_json}"
      artifacts
      ${_artifact_index}
      module_pathname_override)
  endif()

  list(APPEND _artifact_names
    "${_artifact_${_artifact_index}_name}")
  list(APPEND _artifact_controls
    "${_artifact_${_artifact_index}_control}")
  list(APPEND _artifact_install_sqls
    "${_artifact_${_artifact_index}_install_sql}")
  list(APPEND _artifact_libraries
    "${_artifact_${_artifact_index}_library}")
  list(APPEND _artifact_module_pathname_overrides
    "${_module_pathname_override}")
endforeach()

set(_seen_extension_names)
set(_seen_library_filenames)
set(_artifact_versions)
math(EXPR _last_artifact_index "${_artifact_count} - 1")
foreach(_artifact_index RANGE 0 ${_last_artifact_index})
  list(GET _artifact_names ${_artifact_index} _extension_name)
  list(GET _artifact_controls ${_artifact_index} _control_file)
  list(GET _artifact_install_sqls ${_artifact_index} _install_sql)
  list(GET _artifact_libraries ${_artifact_index} _library_file)
  list(GET
    _artifact_module_pathname_overrides
    ${_artifact_index}
    _module_pathname_override)

  if(NOT _extension_name MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR
      "stage_private_postgres: invalid extension name "
      "'${_extension_name}'")
  endif()
  if(_extension_name IN_LIST _seen_extension_names)
    message(FATAL_ERROR
      "stage_private_postgres: duplicate extension name "
      "'${_extension_name}'")
  endif()
  list(APPEND _seen_extension_names "${_extension_name}")

  foreach(_artifact_path IN ITEMS
      _control_file _install_sql _library_file)
    if(NOT IS_ABSOLUTE "${${_artifact_path}}")
      message(FATAL_ERROR
        "stage_private_postgres: ${_extension_name} artifact path must be "
        "absolute: ${${_artifact_path}}")
    endif()
    if(NOT EXISTS "${${_artifact_path}}"
       OR IS_DIRECTORY "${${_artifact_path}}")
      message(FATAL_ERROR
        "stage_private_postgres: ${_extension_name} artifact is not a file: "
        "${${_artifact_path}}")
    endif()
    if(IS_SYMLINK "${${_artifact_path}}")
      message(FATAL_ERROR
        "stage_private_postgres: ${_extension_name} artifact must "
        "not be a symbolic link: ${${_artifact_path}}")
    endif()
  endforeach()

  get_filename_component(_control_filename "${_control_file}" NAME)
  if(NOT _control_filename STREQUAL "${_extension_name}.control")
    message(FATAL_ERROR
      "stage_private_postgres: ${_extension_name} control basename must be "
      "${_extension_name}.control, got ${_control_filename}")
  endif()
  get_filename_component(_install_sql_name "${_install_sql}" NAME)
  if(NOT _install_sql_name MATCHES
      "^${_extension_name}--([A-Za-z0-9][A-Za-z0-9._+-]*)\\.sql$")
    message(FATAL_ERROR
      "stage_private_postgres: ${_extension_name} install SQL must use "
      "canonical basename ${_extension_name}--<version>.sql, got "
      "${_install_sql_name}")
  endif()
  list(APPEND _artifact_versions "${CMAKE_MATCH_1}")

  get_filename_component(_library_filename "${_library_file}" NAME)
  if(_library_filename IN_LIST _seen_library_filenames)
    message(FATAL_ERROR
      "stage_private_postgres: duplicate staged library basename "
      "'${_library_filename}'")
  endif()
  list(APPEND _seen_library_filenames "${_library_filename}")
  if(NOT _module_pathname_override STREQUAL "-"
     AND NOT _module_pathname_override MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR
      "stage_private_postgres: ${_extension_name} "
      "module_pathname_override must be a bare lower-case library name")
  endif()
endforeach()

# Query the selected pg_config for the layout and provenance.
function(_pg_config_value _flag _out)
  execute_process(
    COMMAND "${PG_CONFIG}" "${_flag}"
    OUTPUT_VARIABLE _value
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "stage_private_postgres: ${PG_CONFIG} ${_flag} failed")
  endif()
  set(${_out} "${_value}" PARENT_SCOPE)
endfunction()

_pg_config_value(--bindir PG_BINDIR)
_pg_config_value(--sharedir PG_SHAREDIR)
_pg_config_value(--pkglibdir PG_PKGLIBDIR)
_pg_config_value(--version PG_VERSION)
_pg_config_value(--configure PG_CONFIGURE)
if(NOT PG_VERSION MATCHES "^PostgreSQL 17([.]|[ \t]|$)")
  message(FATAL_ERROR
    "stage_private_postgres: PostgreSQL 17 is required, got ${PG_VERSION}")
endif()
foreach(_postgres_directory IN ITEMS PG_BINDIR PG_SHAREDIR PG_PKGLIBDIR)
  if(NOT IS_ABSOLUTE "${${_postgres_directory}}"
     OR NOT IS_DIRECTORY "${${_postgres_directory}}")
    message(FATAL_ERROR
      "stage_private_postgres: pg_config returned invalid "
      "${_postgres_directory}: ${${_postgres_directory}}")
  endif()
  set(_postgres_directory_normal "${${_postgres_directory}}")
  cmake_path(NORMAL_PATH _postgres_directory_normal)
  if(NOT "${${_postgres_directory}}" STREQUAL
      "${_postgres_directory_normal}")
    message(FATAL_ERROR
      "stage_private_postgres: pg_config returned a noncanonical "
      "${_postgres_directory}: ${${_postgres_directory}}")
  endif()
endforeach()

string(REGEX REPLACE "^/+" "" _bindir_rel "${PG_BINDIR}")

# Reproduce an absolute source path under STAGE_ROOT.
function(_staged_path _absolute _out)
  string(REGEX REPLACE "^/+" "" _relative "${_absolute}")
  if(DEFINED _stage_root_normal)
    set(_stage_base "${_stage_root_normal}")
  else()
    cmake_path(
      ABSOLUTE_PATH STAGE_ROOT NORMALIZE OUTPUT_VARIABLE _stage_base)
  endif()
  set(_destination "${_stage_base}/${_relative}")
  cmake_path(NORMAL_PATH _destination)
  file(RELATIVE_PATH _stage_relative "${_stage_base}" "${_destination}")
  if(_stage_relative STREQUAL ""
     OR _stage_relative STREQUAL ".."
     OR _stage_relative MATCHES "^[.][.]/"
     OR IS_ABSOLUTE "${_stage_relative}")
    message(FATAL_ERROR
      "stage_private_postgres: staged coordinate escapes STAGE_ROOT: "
      "${_absolute}")
  endif()
  set(${_out} "${_destination}" PARENT_SCOPE)
endfunction()

function(_json_escape _input _output)
  set(_escaped "${_input}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  string(REPLACE "\r" "\\r" _escaped "${_escaped}")
  string(REPLACE "\n" "\\n" _escaped "${_escaped}")
  string(REPLACE "\t" "\\t" _escaped "${_escaped}")
  set(${_output} "${_escaped}" PARENT_SCOPE)
endfunction()

function(_path_is_within _candidate _root _output)
  file(RELATIVE_PATH _relative "${_root}" "${_candidate}")
  if(_relative STREQUAL ""
     OR (NOT _relative STREQUAL ".."
         AND NOT _relative MATCHES "^[.][.]/"
         AND NOT IS_ABSOLUTE "${_relative}"))
    set(${_output} TRUE PARENT_SCOPE)
  else()
    set(${_output} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(_resolve_runtime_link
    _path _canonical_root _label _link_target_out _resolved_out)
  file(READ_SYMLINK "${_path}" _initial_link_target)
  set(_current "${_path}")
  set(_visited_links "")
  foreach(_link_depth RANGE 0 256)
    if(IS_SYMLINK "${_current}")
      list(FIND _visited_links "${_current}" _visited_index)
      if(NOT _visited_index EQUAL -1)
        message(FATAL_ERROR
          "stage_private_postgres: runtime link cycle in ${_label}: "
          "${_path}")
      endif()
      list(APPEND _visited_links "${_current}")
      file(READ_SYMLINK "${_current}" _next_link_target)
      if(IS_ABSOLUTE "${_next_link_target}")
        set(_next "${_next_link_target}")
      else()
        get_filename_component(_current_parent "${_current}" DIRECTORY)
        set(_next "${_current_parent}/${_next_link_target}")
      endif()
      cmake_path(NORMAL_PATH _next)
      _path_is_within("${_next}" "${_canonical_root}" _next_is_owned)
      if(NOT _next_is_owned)
        message(FATAL_ERROR
          "stage_private_postgres: runtime link escapes canonical package "
          "root in ${_label}: ${_current} -> ${_next}")
      endif()
      set(_current "${_next}")
      continue()
    endif()
    if(NOT EXISTS "${_current}")
      message(FATAL_ERROR
        "stage_private_postgres: dangling runtime link in ${_label}: "
        "${_path} -> ${_current}")
    endif()
    file(REAL_PATH "${_current}" _resolved)
    _path_is_within("${_resolved}" "${_canonical_root}" _resolved_is_owned)
    if(NOT _resolved_is_owned)
      message(FATAL_ERROR
        "stage_private_postgres: runtime link escapes canonical package root "
        "in ${_label}: ${_path} -> ${_resolved}")
    endif()
    set(${_link_target_out} "${_initial_link_target}" PARENT_SCOPE)
    set(${_resolved_out} "${_resolved}" PARENT_SCOPE)
    return()
  endforeach()
  message(FATAL_ERROR
    "stage_private_postgres: runtime link cycle/depth overflow in ${_label}: "
    "${_path}")
endfunction()

function(_require_runtime_text _value _label)
  if("${_value}" MATCHES ";")
    message(FATAL_ERROR
      "stage_private_postgres: ${_label} contains a forbidden record "
      "delimiter")
  endif()
  foreach(_control_code RANGE 1 31)
    string(ASCII ${_control_code} _control_character)
    string(FIND "${_value}" "${_control_character}" _control_index)
    if(NOT _control_index EQUAL -1)
      message(FATAL_ERROR
        "stage_private_postgres: ${_label} contains an unsupported control "
        "byte")
    endif()
  endforeach()
endfunction()

function(_require_regular_runtime_file _path _label)
  if(NOT EXISTS "${_path}"
     OR IS_DIRECTORY "${_path}"
     OR IS_SYMLINK "${_path}")
    message(FATAL_ERROR
      "stage_private_postgres: ${_label} must resolve to a regular file: "
      "${_path}")
  endif()
  find_program(_bioseq_stat_command NAMES stat REQUIRED)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(
      COMMAND "${_bioseq_stat_command}" -f "%HT" "${_path}"
      OUTPUT_VARIABLE _file_type
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _stat_rc)
    set(_expected_type "Regular File")
  elseif(CMAKE_HOST_UNIX)
    execute_process(
      COMMAND "${_bioseq_stat_command}" -c "%F" -- "${_path}"
      OUTPUT_VARIABLE _file_type
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _stat_rc)
    set(_expected_type "regular file")
  else()
    message(FATAL_ERROR
      "stage_private_postgres: runtime materialization supports POSIX hosts")
  endif()
  if(NOT _stat_rc EQUAL 0 OR NOT _file_type STREQUAL _expected_type)
    message(FATAL_ERROR
      "stage_private_postgres: ${_label} is a special or unreadable file: "
      "${_path} (${_file_type})")
  endif()
endfunction()

function(_runtime_coordinate_is_excluded _root _relative _output)
  get_property(
    _excluded GLOBAL PROPERTY BIOSEQ_RUNTIME_EXCLUDED_COORDINATES)
  list(FIND _excluded "${_root}|${_relative}" _excluded_index)
  if(_excluded_index EQUAL -1)
    set(${_output} FALSE PARENT_SCOPE)
  else()
    set(${_output} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(_append_runtime_entry
    _root
    _relative
    _kind
    _source_kind
    _source_path
    _source_link_target
    _resolved_source_path
    _size
    _sha256)
  _require_runtime_text("${_root}" "runtime root")
  _require_runtime_text("${_relative}" "runtime relative path")
  _require_runtime_text("${_source_path}" "runtime source path")
  _require_runtime_text(
    "${_source_link_target}" "runtime source link target")
  _require_runtime_text(
    "${_resolved_source_path}" "runtime resolved source path")
  set(_normalized_relative "${_relative}")
  cmake_path(NORMAL_PATH _normalized_relative)
  if(IS_ABSOLUTE "${_relative}"
     OR "${_relative}" STREQUAL ""
     OR "${_relative}" STREQUAL ".."
     OR "${_relative}" MATCHES "^[.][.]/"
     OR NOT "${_relative}" STREQUAL "${_normalized_relative}")
    message(FATAL_ERROR
      "stage_private_postgres: noncanonical runtime coordinate "
      "${_root}:${_relative}")
  endif()

  set(_entry_key "${_root}|${_relative}")
  get_property(_entry_keys GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_KEYS)
  list(FIND _entry_keys "${_entry_key}" _entry_index)
  if(NOT _entry_index EQUAL -1)
    message(FATAL_ERROR
      "stage_private_postgres: runtime coordinate collision: ${_entry_key}")
  endif()
  set_property(
    GLOBAL APPEND PROPERTY BIOSEQ_RUNTIME_ENTRY_KEYS "${_entry_key}")

  _json_escape("${_root}" _root_json)
  _json_escape("${_relative}" _relative_json)
  _json_escape("${_kind}" _kind_json)
  _json_escape("${_source_kind}" _source_kind_json)
  _json_escape("${_source_path}" _source_path_json)
  _json_escape("${_resolved_source_path}" _resolved_source_path_json)
  if("${_source_link_target}" STREQUAL "")
    set(_source_link_target_json "null")
    set(_source_link_target_record "-")
  else()
    _json_escape("${_source_link_target}" _source_link_target_escaped)
    set(_source_link_target_json
      "\"${_source_link_target_escaped}\"")
    set(_source_link_target_record "${_source_link_target}")
  endif()
  if(_kind STREQUAL "dir")
    set(_size_json "null")
    set(_sha256_json "null")
    set(_size_record "-")
    set(_sha256_record "-")
  else()
    set(_size_json "${_size}")
    set(_sha256_json "\"${_sha256}\"")
    set(_size_record "${_size}")
    set(_sha256_record "${_sha256}")
  endif()

  string(CONCAT _entry_json
    "      {\"root\": \"${_root_json}\", "
    "\"path\": \"${_relative_json}\", "
    "\"kind\": \"${_kind_json}\", "
    "\"source_kind\": \"${_source_kind_json}\", "
    "\"source_path\": \"${_source_path_json}\", "
    "\"source_link_target\": ${_source_link_target_json}, "
    "\"resolved_source_path\": \"${_resolved_source_path_json}\", "
    "\"size\": ${_size_json}, \"sha256\": ${_sha256_json}}")
  string(CONCAT _entry_record
    # Canonical aggregate framing: concatenate these UTF-8 newline records
    # in bindir/sharedir/pkglibdir enum order and then normalized path order.
    # There is deliberately no header, separator, or trailer beyond each
    # entry's terminal newline.
    "root:${_root}\n"
    "path:${_relative}\n"
    "kind:${_kind}\n"
    "source_kind:${_source_kind}\n"
    "source_path:${_source_path}\n"
    "source_link_target:${_source_link_target_record}\n"
    "resolved_source_path:${_resolved_source_path}\n"
    "size:${_size_record}\n"
    "sha256:${_sha256_record}\n")
  string(SHA256 _entry_property_suffix "${_entry_key}")
  set_property(
    GLOBAL PROPERTY
    "BIOSEQ_RUNTIME_ENTRY_JSON_${_entry_property_suffix}" "${_entry_json}")
  set_property(
    GLOBAL PROPERTY
    "BIOSEQ_RUNTIME_ENTRY_RECORD_${_entry_property_suffix}" "${_entry_record}")
  get_property(_entry_count GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_COUNT)
  math(EXPR _entry_count "${_entry_count} + 1")
  set_property(GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_COUNT "${_entry_count}")
endfunction()

function(_seal_runtime_entries _json_out _records_out)
  get_property(_entry_keys GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_KEYS)
  set(_entries_json "")
  set(_records "")
  # This explicit enum order is the manifest's canonical (root, path) order.
  foreach(_root IN ITEMS bindir sharedir pkglibdir)
    set(_root_keys "")
    foreach(_entry_key IN LISTS _entry_keys)
      if(_entry_key MATCHES "^${_root}[|]")
        list(APPEND _root_keys "${_entry_key}")
      endif()
    endforeach()
    list(SORT _root_keys)
    foreach(_entry_key IN LISTS _root_keys)
      string(SHA256 _entry_property_suffix "${_entry_key}")
      get_property(
        _entry_json GLOBAL PROPERTY
        "BIOSEQ_RUNTIME_ENTRY_JSON_${_entry_property_suffix}")
      get_property(
        _entry_record GLOBAL PROPERTY
        "BIOSEQ_RUNTIME_ENTRY_RECORD_${_entry_property_suffix}")
      if(NOT "${_entries_json}" STREQUAL "")
        string(APPEND _entries_json ",\n")
      endif()
      string(APPEND _entries_json "${_entry_json}")
      string(APPEND _records "${_entry_record}")
    endforeach()
  endforeach()
  set(${_json_out} "${_entries_json}" PARENT_SCOPE)
  set(${_records_out} "${_records}" PARENT_SCOPE)
endfunction()

function(_materialize_runtime_entry
    _root
    _reported_root
    _canonical_root
    _source_path
    _relative
    _destination_root
    _ancestor_directories)
  _runtime_coordinate_is_excluded(
    "${_root}" "${_relative}" _coordinate_is_excluded)
  if(_coordinate_is_excluded)
    return()
  endif()

  if(IS_SYMLINK "${_source_path}")
    _resolve_runtime_link(
      "${_source_path}"
      "${_canonical_root}"
      "canonical ${_root}"
      _source_link_target
      _resolved_source)
    set(_source_kind "symlink")
  else()
    set(_source_link_target "")
    if(NOT EXISTS "${_source_path}")
      message(FATAL_ERROR
        "stage_private_postgres: runtime source disappeared: ${_source_path}")
    endif()
    file(REAL_PATH "${_source_path}" _resolved_source)
    if(IS_DIRECTORY "${_source_path}")
      set(_source_kind "dir")
    else()
      set(_source_kind "file")
    endif()
  endif()

  _path_is_within(
    "${_resolved_source}" "${_canonical_root}" _source_is_owned)
  if(NOT _source_is_owned)
    message(FATAL_ERROR
      "stage_private_postgres: runtime link escapes canonical ${_root}: "
      "${_source_path} -> ${_resolved_source}")
  endif()

  set(_reported_coordinate "${_reported_root}/${_relative}")
  if(IS_SYMLINK "${_reported_coordinate}")
    _resolve_runtime_link(
      "${_reported_coordinate}"
      "${_canonical_root}"
      "reported ${_root}"
      _reported_link_target
      _reported_resolved)
    if(NOT "${_reported_resolved}" STREQUAL "${_resolved_source}")
      message(FATAL_ERROR
        "stage_private_postgres: reported/canonical runtime collision at "
        "${_root}:${_relative}: ${_reported_resolved} != "
        "${_resolved_source}")
    endif()
    set(_source_kind "symlink")
    set(_source_link_target "${_reported_link_target}")
    set(_source_coordinate "${_reported_coordinate}")
  elseif(EXISTS "${_reported_coordinate}"
      AND NOT IS_DIRECTORY "${_reported_coordinate}")
    file(REAL_PATH "${_reported_coordinate}" _reported_resolved)
    if(NOT "${_reported_resolved}" STREQUAL "${_resolved_source}")
      message(FATAL_ERROR
        "stage_private_postgres: reported/canonical runtime collision at "
        "${_root}:${_relative}: ${_reported_resolved} != "
        "${_resolved_source}")
    endif()
    set(_source_kind "file")
    set(_source_link_target "")
    set(_source_coordinate "${_reported_coordinate}")
  elseif(IS_DIRECTORY "${_reported_coordinate}"
      AND NOT IS_DIRECTORY "${_resolved_source}")
    message(FATAL_ERROR
      "stage_private_postgres: reported/canonical runtime type collision at "
      "${_root}:${_relative}")
  else()
    # Homebrew's façade contains real container directories whose children
    # link into the Cellar. The container itself does not identify a canonical
    # package directory, so attest the canonical directory as its source.
    set(_source_coordinate "${_source_path}")
  endif()

  set(_destination "${_destination_root}/${_relative}")
  if(IS_DIRECTORY "${_resolved_source}")
    list(FIND
      _ancestor_directories "${_resolved_source}" _cycle_index)
    if(NOT _cycle_index EQUAL -1)
      message(FATAL_ERROR
        "stage_private_postgres: runtime directory link cycle at "
        "${_source_path}")
    endif()
    if(EXISTS "${_destination}" OR IS_SYMLINK "${_destination}")
      message(FATAL_ERROR
        "stage_private_postgres: runtime destination collision: "
        "${_destination}")
    endif()
    file(MAKE_DIRECTORY "${_destination}")
    _append_runtime_entry(
      "${_root}" "${_relative}" "dir" "${_source_kind}"
      "${_source_coordinate}" "${_source_link_target}"
      "${_resolved_source}" "" "")

    set(_next_ancestors ${_ancestor_directories})
    list(APPEND _next_ancestors "${_resolved_source}")
    file(GLOB _children
      LIST_DIRECTORIES TRUE
      "${_resolved_source}/*"
      "${_resolved_source}/.*")
    list(REMOVE_DUPLICATES _children)
    list(SORT _children)
    foreach(_child IN LISTS _children)
      get_filename_component(_child_name "${_child}" NAME)
      if(_child_name STREQUAL "." OR _child_name STREQUAL "..")
        continue()
      endif()
      _materialize_runtime_entry(
        "${_root}"
        "${_reported_root}"
        "${_canonical_root}"
        "${_child}"
        "${_relative}/${_child_name}"
        "${_destination_root}"
        "${_next_ancestors}")
    endforeach()
    return()
  endif()

  _require_regular_runtime_file(
    "${_resolved_source}" "canonical ${_root} runtime entry")
  if(EXISTS "${_destination}" OR IS_SYMLINK "${_destination}")
    message(FATAL_ERROR
      "stage_private_postgres: runtime destination collision: ${_destination}")
  endif()
  get_filename_component(_destination_parent "${_destination}" DIRECTORY)
  get_filename_component(_destination_name "${_destination}" NAME)
  get_filename_component(_resolved_name "${_resolved_source}" NAME)
  file(MAKE_DIRECTORY "${_destination_parent}")
  if(_destination_name STREQUAL _resolved_name)
    file(COPY "${_resolved_source}"
      DESTINATION "${_destination_parent}"
      USE_SOURCE_PERMISSIONS)
  else()
    file(COPY_FILE "${_resolved_source}" "${_destination}")
    if(_root STREQUAL "bindir")
      file(CHMOD "${_destination}" PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)
    endif()
  endif()
  if(IS_SYMLINK "${_destination}" OR NOT EXISTS "${_destination}")
    message(FATAL_ERROR
      "stage_private_postgres: runtime entry was not materialized as a file: "
      "${_destination}")
  endif()
  _require_regular_runtime_file(
    "${_destination}" "materialized ${_root} runtime entry")
  file(SIZE "${_destination}" _destination_size)
  file(SHA256 "${_resolved_source}" _source_sha)
  file(SHA256 "${_destination}" _destination_sha)
  if(NOT _source_sha STREQUAL _destination_sha)
    message(FATAL_ERROR
      "stage_private_postgres: materialized runtime digest mismatch: "
      "${_destination}")
  endif()
  _append_runtime_entry(
    "${_root}" "${_relative}" "file" "${_source_kind}"
    "${_source_coordinate}" "${_source_link_target}"
    "${_resolved_source}" "${_destination_size}" "${_destination_sha}")
endfunction()

function(_materialize_runtime_root
    _root _reported_root _canonical_root _destination_root)
  file(MAKE_DIRECTORY "${_destination_root}")
  file(GLOB _children
    LIST_DIRECTORIES TRUE
    "${_canonical_root}/*"
    "${_canonical_root}/.*")
  list(REMOVE_DUPLICATES _children)
  list(SORT _children)
  foreach(_child IN LISTS _children)
    get_filename_component(_child_name "${_child}" NAME)
    if(_child_name STREQUAL "." OR _child_name STREQUAL "..")
      continue()
    endif()
    _materialize_runtime_entry(
      "${_root}"
      "${_reported_root}"
      "${_canonical_root}"
      "${_child}"
      "${_child_name}"
      "${_destination_root}"
      "${_canonical_root}")
  endforeach()
endfunction()

# Resolve package-owned trees from files PostgreSQL itself must ship. This
# avoids mistaking a package-manager façade (which can contain unrelated
# globally linked extensions) for the selected PostgreSQL package.
foreach(_postgres_text IN ITEMS
    PG_VERSION PG_CONFIGURE PG_BINDIR PG_SHAREDIR PG_PKGLIBDIR)
  _require_runtime_text(
    "${${_postgres_text}}" "pg_config ${_postgres_text}")
endforeach()
foreach(_artifact_path_list IN ITEMS
    _artifact_controls _artifact_install_sqls _artifact_libraries)
  foreach(_artifact_path IN LISTS ${_artifact_path_list})
    _require_runtime_text("${_artifact_path}" "extension artifact path")
  endforeach()
endforeach()
file(REAL_PATH "${PG_BINDIR}" _canonical_bindir)
get_filename_component(
  _canonical_postgres_package_root "${_canonical_bindir}" DIRECTORY)

set(_postgres_bki_reported "${PG_SHAREDIR}/postgres.bki")
if(NOT EXISTS "${_postgres_bki_reported}"
   OR IS_DIRECTORY "${_postgres_bki_reported}")
  message(FATAL_ERROR
    "stage_private_postgres: PostgreSQL 17 postgres.bki sentinel is missing: "
    "${_postgres_bki_reported}")
endif()
file(REAL_PATH "${_postgres_bki_reported}" _postgres_bki_source)
_require_regular_runtime_file(
  "${_postgres_bki_source}" "PostgreSQL 17 postgres.bki sentinel")
get_filename_component(_canonical_sharedir "${_postgres_bki_source}" DIRECTORY)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(_plpgsql_suffix ".dylib")
elseif(CMAKE_HOST_UNIX)
  set(_plpgsql_suffix ".so")
else()
  message(FATAL_ERROR
    "stage_private_postgres: unsupported PostgreSQL module platform")
endif()
file(GLOB _plpgsql_candidates
  LIST_DIRECTORIES FALSE
  "${PG_PKGLIBDIR}/plpgsql${_plpgsql_suffix}")
list(LENGTH _plpgsql_candidates _plpgsql_candidate_count)
if(NOT _plpgsql_candidate_count EQUAL 1)
  message(FATAL_ERROR
    "stage_private_postgres: PostgreSQL 17 requires exactly one platform "
    "plpgsql module in ${PG_PKGLIBDIR}, found ${_plpgsql_candidate_count}")
endif()
list(GET _plpgsql_candidates 0 _plpgsql_reported)
file(REAL_PATH "${_plpgsql_reported}" _plpgsql_source)
_require_regular_runtime_file(
  "${_plpgsql_source}" "PostgreSQL 17 plpgsql module")
get_filename_component(_canonical_pkglibdir "${_plpgsql_source}" DIRECTORY)

foreach(_canonical_directory IN ITEMS
    _canonical_bindir _canonical_sharedir _canonical_pkglibdir)
  if(NOT IS_DIRECTORY "${${_canonical_directory}}"
     OR IS_SYMLINK "${${_canonical_directory}}")
    message(FATAL_ERROR
      "stage_private_postgres: canonical PostgreSQL package root is invalid: "
      "${${_canonical_directory}}")
  endif()
endforeach()

# A linked sentinel is a package-manager ownership assertion. Its target must
# remain in the package selected by the resolved PostgreSQL bindir. Direct
# distro layouts (for example /usr/lib + /usr/share) remain supported.
foreach(_linked_sentinel IN ITEMS
    "_postgres_bki_reported|${_canonical_sharedir}|sharedir"
    "_plpgsql_reported|${_canonical_pkglibdir}|pkglibdir")
  string(REPLACE "|" ";" _sentinel_parts "${_linked_sentinel}")
  list(GET _sentinel_parts 0 _sentinel_variable)
  list(GET _sentinel_parts 1 _sentinel_root)
  list(GET _sentinel_parts 2 _sentinel_label)
  if(IS_SYMLINK "${${_sentinel_variable}}")
    _path_is_within(
      "${_sentinel_root}"
      "${_canonical_postgres_package_root}"
      _sentinel_is_package_owned)
    if(NOT _sentinel_is_package_owned)
      message(FATAL_ERROR
        "stage_private_postgres: external-package ${_sentinel_label} "
        "sentinel: ${${_sentinel_variable}} -> ${_sentinel_root}")
    endif()
  endif()
endforeach()

string(REGEX REPLACE "^/+" "" _sharedir_rel "${PG_SHAREDIR}")
string(REGEX REPLACE "^/+" "" _pkglibdir_rel "${PG_PKGLIBDIR}")

set(_runtime_excluded_coordinates "")
set(_runtime_overlay_library_coordinates "")
set(_runtime_overlay_library_sources "")
foreach(_artifact_index RANGE 0 ${_last_artifact_index})
  list(GET _artifact_controls ${_artifact_index} _control_file)
  list(GET _artifact_install_sqls ${_artifact_index} _install_sql)
  list(GET _artifact_libraries ${_artifact_index} _library_file)
  get_filename_component(_control_filename "${_control_file}" NAME)
  get_filename_component(_install_sql_name "${_install_sql}" NAME)
  get_filename_component(_library_filename "${_library_file}" NAME)
  file(REAL_PATH "${_library_file}" _library_source_real)
  foreach(_overlay_source IN ITEMS
      "${_canonical_sharedir}/extension/${_control_filename}"
      "${_canonical_sharedir}/extension/${_install_sql_name}"
      "${_canonical_pkglibdir}/${_library_filename}")
    if(IS_DIRECTORY "${_overlay_source}")
      message(FATAL_ERROR
        "stage_private_postgres: extension overlay coordinate collision: "
        "${_overlay_source}")
    elseif(EXISTS "${_overlay_source}" AND NOT IS_SYMLINK "${_overlay_source}")
      _require_regular_runtime_file(
        "${_overlay_source}" "extension overlay source coordinate")
    endif()
  endforeach()
  list(APPEND _runtime_excluded_coordinates
    "sharedir|extension/${_control_filename}"
    "sharedir|extension/${_install_sql_name}"
    "pkglibdir|${_library_filename}")
  list(APPEND
    _runtime_overlay_library_coordinates "pkglibdir|${_library_filename}")
  list(APPEND _runtime_overlay_library_sources "${_library_source_real}")
endforeach()
list(REMOVE_DUPLICATES _runtime_excluded_coordinates)
set_property(
  GLOBAL PROPERTY BIOSEQ_RUNTIME_EXCLUDED_COORDINATES
  "${_runtime_excluded_coordinates}")

# These are the server/client programs consumed by the private-cluster runner.
# Keep this list shared by runtime-closure discovery and the validation below so
# a newly required tool cannot be validated without also having its private
# runtime dependencies staged.
set(_tools postgres initdb pg_ctl createdb dropdb psql)

# Fresh stage on every run — the prefix is an incremental build artifact.
# This guard is intentionally adjacent to the destructive operation. The
# Python lifecycle prevalidates the same parent, but this script also fails
# closed when invoked directly or when a path is replaced between layers.
cmake_path(
  ABSOLUTE_PATH STAGE_PARENT
  NORMALIZE
  OUTPUT_VARIABLE _stage_parent_normal)
cmake_path(
  ABSOLUTE_PATH STAGE_ROOT
  NORMALIZE
  OUTPUT_VARIABLE _stage_root_normal)
get_filename_component(_stage_root_parent "${_stage_root_normal}" DIRECTORY)
if(NOT "${_stage_root_parent}" STREQUAL "${_stage_parent_normal}")
  message(FATAL_ERROR
    "stage_private_postgres: STAGE_ROOT must be a direct child of "
    "STAGE_PARENT: ${_stage_root_normal}")
endif()
if(NOT IS_DIRECTORY "${_stage_parent_normal}"
   OR IS_SYMLINK "${_stage_parent_normal}")
  message(FATAL_ERROR
    "stage_private_postgres: STAGE_PARENT must be a real directory: "
    "${_stage_parent_normal}")
endif()
file(REAL_PATH "${_stage_parent_normal}" _stage_parent_real)
if(NOT "${_stage_parent_real}" STREQUAL "${_stage_parent_normal}")
  message(FATAL_ERROR
    "stage_private_postgres: STAGE_PARENT contains a symbolic-link "
    "component: ${_stage_parent_normal}")
endif()
if(EXISTS "${_stage_root_normal}" OR IS_SYMLINK "${_stage_root_normal}")
  if(NOT IS_DIRECTORY "${_stage_root_normal}"
     OR IS_SYMLINK "${_stage_root_normal}")
    message(FATAL_ERROR
      "stage_private_postgres: STAGE_ROOT must be a real directory when "
      "it exists: ${_stage_root_normal}")
  endif()
  file(REAL_PATH "${_stage_root_normal}" _stage_root_real)
  if(NOT "${_stage_root_real}" STREQUAL "${_stage_root_normal}")
    message(FATAL_ERROR
      "stage_private_postgres: STAGE_ROOT contains a symbolic-link "
      "component: ${_stage_root_normal}")
  endif()
endif()
# Invalidate receipts before replacing the tree. A failed restage must never
# leave an old attestation beside a deleted or partially materialized runtime.
set(_manifest_temporary "${MANIFEST_OUT}.tmp")
set(_hash_temporary "${HASH_OUT}.tmp")
file(REMOVE
  "${MANIFEST_OUT}"
  "${HASH_OUT}"
  "${_manifest_temporary}"
  "${_hash_temporary}")
file(REMOVE_RECURSE "${_stage_root_normal}")
set(STAGE_ROOT "${_stage_root_normal}")
_staged_path("${PG_BINDIR}" STAGED_BINDIR)
_staged_path("${PG_SHAREDIR}" STAGED_SHAREDIR)
_staged_path("${PG_PKGLIBDIR}" STAGED_PKGLIBDIR)

# Materialize only the package-owned roots into pg_config's reported
# coordinates. Every link is resolved and copied as a real directory or
# regular file, while exact extension-artifact destinations are reserved for the
# overlay below and therefore omitted from this closed runtime attestation.
set_property(GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_KEYS "")
set_property(GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_COUNT "0")
_materialize_runtime_root(
  bindir
  "${PG_BINDIR}"
  "${_canonical_bindir}"
  "${STAGED_BINDIR}")
_materialize_runtime_root(
  sharedir
  "${PG_SHAREDIR}"
  "${_canonical_sharedir}"
  "${STAGED_SHAREDIR}")
_materialize_runtime_root(
  pkglibdir
  "${PG_PKGLIBDIR}"
  "${_canonical_pkglibdir}"
  "${STAGED_PKGLIBDIR}")
_seal_runtime_entries(_runtime_entries_json _runtime_entry_records)
get_property(
  _runtime_entry_count GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_COUNT)
string(SHA256 _runtime_aggregate_sha "${_runtime_entry_records}")

# A PostgreSQL distribution may keep support libraries one level above
# pkglibdir.  Postgres.app is one concrete example: postgres lives in bin/ and
# loads lib/libzstd.1.dylib via @loader_path/../lib, while pg_config reports
# lib/postgresql as pkglibdir.  Copying only pkglibdir therefore leaves the
# otherwise-relocatable staged server unable to start.
#
# Discover the exact transitive dependency closure of the tools (and of the
# current extension DSO), then copy only dependencies owned by a plausible
# sibling PostgreSQL runtime directory.  Never copy a complete sibling lib
# tree: it may contain unrelated or globally installed Bioseq artifacts.
set(_runtime_executables "")
foreach(_tool ${_tools})
  list(APPEND _runtime_executables "${PG_BINDIR}/${_tool}")
endforeach()

set(_runtime_roots "${PG_PKGLIBDIR}")
get_filename_component(_bindir_parent "${PG_BINDIR}" DIRECTORY)
foreach(_libdir_name lib lib64)
  if(IS_DIRECTORY "${_bindir_parent}/${_libdir_name}")
    list(APPEND _runtime_roots "${_bindir_parent}/${_libdir_name}")
  endif()
endforeach()
get_filename_component(_pkglib_name "${PG_PKGLIBDIR}" NAME)
if(_pkglib_name MATCHES "^postgresql(@.*)?$")
  get_filename_component(_pkglib_parent "${PG_PKGLIBDIR}" DIRECTORY)
  if(IS_DIRECTORY "${_pkglib_parent}")
    list(APPEND _runtime_roots "${_pkglib_parent}")
  endif()
endif()
list(REMOVE_DUPLICATES _runtime_roots)

file(GET_RUNTIME_DEPENDENCIES
  RESOLVED_DEPENDENCIES_VAR _runtime_dependencies
  UNRESOLVED_DEPENDENCIES_VAR _unresolved_runtime_dependencies
  EXECUTABLES ${_runtime_executables}
  MODULES ${_artifact_libraries})
if(_unresolved_runtime_dependencies)
  list(JOIN _unresolved_runtime_dependencies ", " _unresolved_text)
  message(FATAL_ERROR
    "stage_private_postgres: unresolved runtime dependencies: ${_unresolved_text}")
endif()

# Return a dependency's relative path within a lexical or symlink-resolved
# source root.  The lexical root is retained for the staged destination so an
# @loader_path/$ORIGIN reference sees the same relative layout after copying.
function(_runtime_relative_path _dependency _root _out)
  set(_relative "")
  file(REAL_PATH "${_dependency}" _dependency_real)
  file(REAL_PATH "${_root}" _root_real)
  foreach(_candidate_path "${_dependency}" "${_dependency_real}")
    foreach(_candidate_root "${_root}" "${_root_real}")
      file(RELATIVE_PATH _try_relative "${_candidate_root}" "${_candidate_path}")
      if(NOT _try_relative STREQUAL ".."
          AND NOT _try_relative MATCHES "^[.][.]/"
          AND NOT IS_ABSOLUTE "${_try_relative}")
        set(_relative "${_try_relative}")
        break()
      endif()
    endforeach()
    if(NOT _relative STREQUAL "")
      break()
    endif()
  endforeach()

  # file(GET_RUNTIME_DEPENDENCIES) may canonicalise a symlink into a package
  # manager's cellar.  Match the same file through a direct sibling-lib alias
  # so it is still staged at the loader-relative path.
  if(_relative STREQUAL "")
    # ELF dependency discovery can retain the SONAME symlink (libfoo.so.1),
    # while the package facade names its resolved file (libfoo.so.1.0).
    # Both coordinates can name this same dependency; check both spellings
    # before deciding that the facade has no loader/overlay intersection.
    foreach(_alias_candidate "${_dependency}" "${_dependency_real}")
      get_filename_component(_dependency_name "${_alias_candidate}" NAME)
      set(_alias "${_root}/${_dependency_name}")
      if(EXISTS "${_alias}")
        file(REAL_PATH "${_alias}" _alias_real)
        if("${_alias_real}" STREQUAL "${_dependency_real}")
          set(_relative "${_dependency_name}")
          break()
        endif()
      endif()
    endforeach()
  endif()
  set(${_out} "${_relative}" PARENT_SCOPE)
endfunction()

list(SORT _runtime_dependencies)
set(_loader_dependency_sort_keys "")
foreach(_dependency ${_runtime_dependencies})
  set(_runtime_candidate_roots "")
  set(_runtime_candidate_relatives "")
  foreach(_candidate_root ${_runtime_roots})
    _runtime_relative_path(
      "${_dependency}" "${_candidate_root}" _candidate_relative)
    if(NOT _candidate_relative STREQUAL "")
      list(APPEND _runtime_candidate_roots "${_candidate_root}")
      list(APPEND _runtime_candidate_relatives "${_candidate_relative}")
    endif()
  endforeach()
  list(LENGTH _runtime_candidate_roots _runtime_candidate_count)
  if(_runtime_candidate_count EQUAL 0)
    continue()
  endif()

  get_filename_component(_dependency_name "${_dependency}" NAME)
  if(_dependency_name MATCHES "^bioseq")
    message(FATAL_ERROR
      "stage_private_postgres: refusing globally installed Bioseq runtime dependency: ${_dependency}")
  endif()
  file(REAL_PATH "${_dependency}" _dependency_real)
  _require_regular_runtime_file(
    "${_dependency_real}" "PostgreSQL loader dependency")
  math(EXPR _runtime_candidate_last "${_runtime_candidate_count} - 1")
  foreach(_runtime_candidate_index RANGE 0 ${_runtime_candidate_last})
    list(GET
      _runtime_candidate_roots ${_runtime_candidate_index} _runtime_root)
    list(GET
      _runtime_candidate_relatives
      ${_runtime_candidate_index}
      _runtime_relative)
  _staged_path("${_runtime_root}" _staged_runtime_root)
  set(_runtime_relative_normal "${_runtime_relative}")
  cmake_path(NORMAL_PATH _runtime_relative_normal)
  if(IS_ABSOLUTE "${_runtime_relative}"
     OR "${_runtime_relative}" STREQUAL ""
     OR "${_runtime_relative}" STREQUAL ".."
     OR "${_runtime_relative}" MATCHES "^[.][.]/"
     OR NOT "${_runtime_relative}" STREQUAL "${_runtime_relative_normal}")
    message(FATAL_ERROR
      "stage_private_postgres: noncanonical loader dependency coordinate: "
      "${_runtime_relative}")
  endif()
  set(_staged_dependency "${_staged_runtime_root}/${_runtime_relative}")
  set(_staged_dependency_normal "${_staged_dependency}")
  cmake_path(NORMAL_PATH _staged_dependency_normal)
  if(NOT "${_staged_dependency}" STREQUAL "${_staged_dependency_normal}")
    message(FATAL_ERROR
      "stage_private_postgres: noncanonical staged loader dependency: "
      "${_staged_dependency}")
  endif()
  file(RELATIVE_PATH
    _loader_staged_relative "${STAGE_ROOT}" "${_staged_dependency}")
  if(_loader_staged_relative STREQUAL ""
     OR _loader_staged_relative STREQUAL ".."
     OR _loader_staged_relative MATCHES "^[.][.]/"
     OR IS_ABSOLUTE "${_loader_staged_relative}")
    message(FATAL_ERROR
      "stage_private_postgres: loader dependency escapes STAGE_ROOT: "
      "${_staged_dependency}")
  endif()
  set(_loader_overlay_coordinate "")
  foreach(_main_root IN ITEMS bindir sharedir pkglibdir)
    if(_main_root STREQUAL "bindir")
      set(_main_destination_root "${STAGED_BINDIR}")
    elseif(_main_root STREQUAL "sharedir")
      set(_main_destination_root "${STAGED_SHAREDIR}")
    else()
      set(_main_destination_root "${STAGED_PKGLIBDIR}")
    endif()
    file(RELATIVE_PATH
      _main_relative "${_main_destination_root}" "${_staged_dependency}")
    if(NOT _main_relative STREQUAL ".."
       AND NOT _main_relative MATCHES "^[.][.]/"
       AND NOT IS_ABSOLUTE "${_main_relative}")
      set(_loader_overlay_coordinate "${_main_root}|${_main_relative}")
      break()
    endif()
  endforeach()
  list(FIND
    _runtime_excluded_coordinates
    "${_loader_overlay_coordinate}"
    _loader_overlay_index)
  if(NOT _loader_overlay_index EQUAL -1)
    list(FIND
      _runtime_overlay_library_coordinates
      "${_loader_overlay_coordinate}"
      _matching_overlay_library_index)
    set(_matching_overlay_library FALSE)
    if(NOT _matching_overlay_library_index EQUAL -1)
      list(GET
        _runtime_overlay_library_sources
        ${_matching_overlay_library_index}
        _matching_overlay_library_source)
      if("${_dependency_real}" STREQUAL "${_matching_overlay_library_source}")
        set(_matching_overlay_library TRUE)
      endif()
    endif()
    if(_matching_overlay_library)
      # The separately attested overlay will publish these exact bytes.
      continue()
    endif()
    message(FATAL_ERROR
      "stage_private_postgres: loader/overlay coordinate collision: "
      "${_loader_overlay_coordinate}: ${_dependency_real}")
  endif()
  if(EXISTS "${_staged_dependency}" AND NOT IS_SYMLINK "${_staged_dependency}")
    set(_loader_is_in_main_closure FALSE)
    foreach(_main_root IN ITEMS bindir sharedir pkglibdir)
      if(_main_root STREQUAL "bindir")
        set(_main_destination_root "${STAGED_BINDIR}")
      elseif(_main_root STREQUAL "sharedir")
        set(_main_destination_root "${STAGED_SHAREDIR}")
      else()
        set(_main_destination_root "${STAGED_PKGLIBDIR}")
      endif()
      file(RELATIVE_PATH
        _main_relative "${_main_destination_root}" "${_staged_dependency}")
      if(NOT _main_relative STREQUAL ".."
         AND NOT _main_relative MATCHES "^[.][.]/"
         AND NOT IS_ABSOLUTE "${_main_relative}")
        get_property(
          _runtime_entry_keys GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_KEYS)
        list(FIND
          _runtime_entry_keys
          "${_main_root}|${_main_relative}"
          _main_entry_index)
        if(NOT _main_entry_index EQUAL -1)
          set(_loader_is_in_main_closure TRUE)
        endif()
        break()
      endif()
    endforeach()
    if(_loader_is_in_main_closure)
      _require_regular_runtime_file(
        "${_staged_dependency}" "main-closure loader dependency")
      continue()
    endif()
    message(FATAL_ERROR
      "stage_private_postgres: loader dependency destination collision: "
      "${_staged_dependency}")
  endif()
  if(IS_SYMLINK "${_staged_dependency}")
    message(FATAL_ERROR
      "stage_private_postgres: loader dependency destination is a link: "
      "${_staged_dependency}")
  endif()
  get_filename_component(_staged_dependency_dir "${_staged_dependency}" DIRECTORY)
  file(MAKE_DIRECTORY "${_staged_dependency_dir}")
  file(COPY_FILE "${_dependency_real}" "${_staged_dependency}")
  _require_regular_runtime_file(
    "${_staged_dependency}" "materialized PostgreSQL loader dependency")
  file(SIZE "${_staged_dependency}" _loader_size)
  file(SHA256 "${_dependency_real}" _loader_source_sha)
  file(SHA256 "${_staged_dependency}" _loader_staged_sha)
  if(NOT _loader_source_sha STREQUAL _loader_staged_sha)
    message(FATAL_ERROR
      "stage_private_postgres: loader dependency digest mismatch: "
      "${_staged_dependency}")
  endif()
  _require_runtime_text("${_dependency_real}" "loader dependency source")
  _require_runtime_text(
    "${_loader_staged_relative}" "loader dependency staged path")
  # Hex preserves UTF-8 byte order, and '!' sorts before every hex digit.
  # Therefore this injective encoding has true pair ordering, including when
  # one staged path is a prefix of another (for example a before a/b).
  string(HEX "${_loader_staged_relative}" _loader_staged_sort_hex)
  string(HEX "${_dependency_real}" _loader_source_sort_hex)
  set(_loader_sort_key
    "${_loader_staged_sort_hex}!${_loader_source_sort_hex}")
  list(FIND
    _loader_dependency_sort_keys
    "${_loader_sort_key}"
    _loader_key_index)
  if(NOT _loader_key_index EQUAL -1)
    message(FATAL_ERROR
      "stage_private_postgres: duplicate loader dependency pair: "
      "${_loader_staged_relative}, ${_dependency_real}")
  endif()
  list(APPEND _loader_dependency_sort_keys "${_loader_sort_key}")
  _json_escape("${_dependency_real}" _loader_source_json)
  _json_escape("${_loader_staged_relative}" _loader_staged_json)
  string(CONCAT _loader_json
    "        {\"source\": \"${_loader_source_json}\", "
    "\"staged\": \"${_loader_staged_json}\", "
    "\"size\": ${_loader_size}, "
    "\"sha256\": \"${_loader_staged_sha}\"}")
  string(CONCAT _loader_record
    # Canonical loader aggregate framing, sorted by staged then source.
    "source:${_dependency_real}\n"
    "staged:${_loader_staged_relative}\n"
    "size:${_loader_size}\n"
    "sha256:${_loader_staged_sha}\n")
  string(SHA256 _loader_property_suffix "${_loader_sort_key}")
  set_property(
    GLOBAL PROPERTY "BIOSEQ_LOADER_JSON_${_loader_property_suffix}"
    "${_loader_json}")
  set_property(
    GLOBAL PROPERTY "BIOSEQ_LOADER_RECORD_${_loader_property_suffix}"
    "${_loader_record}")
  endforeach()
endforeach()

list(SORT _loader_dependency_sort_keys)
set(_loader_entries_json "")
set(_loader_records "")
foreach(_loader_sort_key IN LISTS _loader_dependency_sort_keys)
  string(SHA256 _loader_property_suffix "${_loader_sort_key}")
  get_property(
    _loader_json GLOBAL PROPERTY "BIOSEQ_LOADER_JSON_${_loader_property_suffix}")
  get_property(
    _loader_record
    GLOBAL PROPERTY "BIOSEQ_LOADER_RECORD_${_loader_property_suffix}")
  if(NOT "${_loader_entries_json}" STREQUAL "")
    string(APPEND _loader_entries_json ",\n")
  endif()
  string(APPEND _loader_entries_json "${_loader_json}")
  string(APPEND _loader_records "${_loader_record}")
endforeach()
list(LENGTH _loader_dependency_sort_keys _loader_entry_count)
string(SHA256 _loader_aggregate_sha "${_loader_records}")

# Add ONLY the current build targets' complete artifact set, never a globally
# installed Bioseq. An override changes only the
# staged control file; source controls and every other staged byte remain
# untouched.
file(MAKE_DIRECTORY "${STAGED_SHAREDIR}/extension")
foreach(_artifact_index RANGE 0 ${_last_artifact_index})
  list(GET _artifact_names ${_artifact_index} _extension_name)
  list(GET _artifact_controls ${_artifact_index} _control_file)
  list(GET _artifact_install_sqls ${_artifact_index} _install_sql)
  list(GET _artifact_libraries ${_artifact_index} _library_file)
  list(GET
    _artifact_module_pathname_overrides
    ${_artifact_index}
    _module_pathname_override)
  list(GET _artifact_versions ${_artifact_index} _extension_version)
  get_filename_component(_control_filename "${_control_file}" NAME)
  get_filename_component(_install_sql_name "${_install_sql}" NAME)
  get_filename_component(_library_filename "${_library_file}" NAME)
  set(_staged_control
    "${STAGED_SHAREDIR}/extension/${_control_filename}")
  set(_staged_install_sql
    "${STAGED_SHAREDIR}/extension/${_install_sql_name}")
  set(_staged_library
    "${STAGED_PKGLIBDIR}/${_library_filename}")

  foreach(_destination IN ITEMS
      _staged_control _staged_install_sql _staged_library)
    if(EXISTS "${${_destination}}" OR IS_SYMLINK "${${_destination}}")
      file(REMOVE "${${_destination}}")
    endif()
  endforeach()
  file(COPY_FILE "${_control_file}" "${_staged_control}")
  file(COPY_FILE "${_install_sql}" "${_staged_install_sql}")
  file(COPY_FILE "${_library_file}" "${_staged_library}")
  _require_regular_runtime_file(
    "${_staged_control}" "staged extension control")
  _require_regular_runtime_file(
    "${_staged_install_sql}" "staged extension install SQL")
  _require_regular_runtime_file(
    "${_staged_library}" "staged extension library")

  file(SHA256 "${_control_file}" _source_control_sha)
  if(NOT _module_pathname_override STREQUAL "-")
    file(READ "${_staged_control}" _staged_control_content)
    set(_module_pathname_pattern
      "(^|\n)([ \t]*)module_pathname[ \t]*=[ \t]*'[^'\r\n]*'")
    file(STRINGS "${_staged_control}" _control_lines)
    set(_module_pathname_key_count 0)
    set(_module_pathname_count 0)
    foreach(_control_line IN LISTS _control_lines)
      if(_control_line MATCHES
          "^[ \t]*module_pathname([^A-Za-z0-9_]|$)")
        math(EXPR
          _module_pathname_key_count
          "${_module_pathname_key_count} + 1")
        if(_control_line MATCHES
            "^[ \t]*module_pathname[ \t]*=[ \t]*'[^']*'[ \t]*(#.*)?$")
          math(EXPR
            _module_pathname_count
            "${_module_pathname_count} + 1")
        endif()
      endif()
    endforeach()
    if(_module_pathname_key_count GREATER 1)
      message(FATAL_ERROR
        "stage_private_postgres: ${_extension_name} control contains "
        "duplicate module_pathname keys: "
        "${_control_file}")
    elseif(_module_pathname_key_count EQUAL 1)
      if(NOT _module_pathname_count EQUAL 1)
        message(FATAL_ERROR
          "stage_private_postgres: ${_extension_name} control contains a "
          "malformed module_pathname assignment: ${_control_file}")
      endif()
      string(REGEX REPLACE
        "${_module_pathname_pattern}"
        "\\1\\2module_pathname = '${_module_pathname_override}'"
        _rewritten_control_content
        "${_staged_control_content}")
    else()
      if(NOT _module_pathname_count EQUAL 0)
        message(FATAL_ERROR
          "stage_private_postgres: ${_extension_name} control "
          "module_pathname parser disagreed about ${_control_file}")
      endif()
      set(_rewritten_control_content "${_staged_control_content}")
      if(NOT _rewritten_control_content STREQUAL ""
         AND NOT _rewritten_control_content MATCHES "\n$")
        string(APPEND _rewritten_control_content "\n")
      endif()
      string(APPEND _rewritten_control_content
        "module_pathname = '${_module_pathname_override}'\n")
    endif()
    file(WRITE "${_staged_control}" "${_rewritten_control_content}")
  endif()

  file(SHA256 "${_staged_control}" _staged_control_sha)
  file(SHA256 "${_install_sql}" _source_sql_sha)
  file(SHA256 "${_staged_install_sql}" _staged_sql_sha)
  file(SHA256 "${_library_file}" _source_library_sha)
  file(SHA256 "${_staged_library}" _staged_library_sha)
  if(NOT _source_sql_sha STREQUAL _staged_sql_sha
     OR NOT _source_library_sha STREQUAL _staged_library_sha)
    message(FATAL_ERROR
      "stage_private_postgres: exact artifact copy verification failed for "
      "${_extension_name}")
  endif()
  if(_module_pathname_override STREQUAL "-"
     AND NOT _source_control_sha STREQUAL _staged_control_sha)
    message(FATAL_ERROR
      "stage_private_postgres: exact control copy verification failed for "
      "${_extension_name}")
  endif()

  set("_artifact_${_artifact_index}_control_filename"
    "${_control_filename}")
  set("_artifact_${_artifact_index}_install_sql_name"
    "${_install_sql_name}")
  set("_artifact_${_artifact_index}_library_filename"
    "${_library_filename}")
  set("_artifact_${_artifact_index}_extension_version"
    "${_extension_version}")
  set("_artifact_${_artifact_index}_source_control_sha"
    "${_source_control_sha}")
  set("_artifact_${_artifact_index}_control_sha"
    "${_staged_control_sha}")
  set("_artifact_${_artifact_index}_sql_sha"
    "${_staged_sql_sha}")
  set("_artifact_${_artifact_index}_library_sha"
    "${_staged_library_sha}")
endforeach()

# Required package sentinels make a superficially populated but incomplete
# PostgreSQL runtime impossible to publish. Every sentinel must also be covered
# by the package closure rather than created by the extension overlay.
set(_required_sentinel_specs
  "postgres_bki|sharedir|postgres.bki|file"
  "postgresql_conf_sample|sharedir|postgresql.conf.sample|file"
  "pg_hba_conf_sample|sharedir|pg_hba.conf.sample|file"
  "pg_ident_conf_sample|sharedir|pg_ident.conf.sample|file"
  "information_schema_sql|sharedir|information_schema.sql|file"
  "system_functions_sql|sharedir|system_functions.sql|file"
  "system_views_sql|sharedir|system_views.sql|file"
  "system_constraints_sql|sharedir|system_constraints.sql|file"
  "snowball_create_sql|sharedir|snowball_create.sql|file"
  "timezone|sharedir|timezone|dir"
  "timezonesets|sharedir|timezonesets|dir"
  "tsearch_data|sharedir|tsearch_data|dir"
  "extension|sharedir|extension|dir"
  "plpgsql|pkglibdir|plpgsql${_plpgsql_suffix}|file")
# Distro PostgreSQL builds can intentionally use the OS tzdata package rather
# than ship sharedir/timezone. Keep this explicit system dependency (recorded
# verbatim in postgres.configure) instead of requiring a nonexistent PG file.
# Other builds still require the materialized timezone directory as before.
separate_arguments(_postgres_configure_flags UNIX_COMMAND "${PG_CONFIGURE}")
foreach(_configure_flag IN LISTS _postgres_configure_flags)
  if(_configure_flag MATCHES "^--with-system-tzdata=(.+)$")
    set(_system_tzdata "${CMAKE_MATCH_1}")
    if(NOT IS_ABSOLUTE "${_system_tzdata}"
       OR NOT IS_DIRECTORY "${_system_tzdata}"
       OR NOT EXISTS "${_system_tzdata}/UTC")
      message(FATAL_ERROR
        "stage_private_postgres: configured system tzdata is unavailable: "
        "${_system_tzdata}")
    endif()
    if(NOT IS_DIRECTORY "${_canonical_sharedir}/timezone")
      list(REMOVE_ITEM _required_sentinel_specs "timezone|sharedir|timezone|dir")
      message(STATUS
        "stage_private_postgres: using configured system tzdata: ${_system_tzdata}")
    endif()
  endif()
endforeach()
set(_required_sentinels_json "")
get_property(_runtime_entry_keys GLOBAL PROPERTY BIOSEQ_RUNTIME_ENTRY_KEYS)
foreach(_sentinel_spec IN LISTS _required_sentinel_specs)
  string(REPLACE "|" ";" _sentinel_parts "${_sentinel_spec}")
  list(GET _sentinel_parts 0 _sentinel_name)
  list(GET _sentinel_parts 1 _sentinel_root)
  list(GET _sentinel_parts 2 _sentinel_path)
  list(GET _sentinel_parts 3 _sentinel_kind)
  if(_sentinel_root STREQUAL "sharedir")
    set(_sentinel_absolute "${STAGED_SHAREDIR}/${_sentinel_path}")
  elseif(_sentinel_root STREQUAL "pkglibdir")
    set(_sentinel_absolute "${STAGED_PKGLIBDIR}/${_sentinel_path}")
  else()
    message(FATAL_ERROR
      "stage_private_postgres: invalid required sentinel root")
  endif()
  list(FIND
    _runtime_entry_keys
    "${_sentinel_root}|${_sentinel_path}"
    _sentinel_runtime_index)
  if(_sentinel_runtime_index EQUAL -1)
    message(FATAL_ERROR
      "stage_private_postgres: required sentinel is outside runtime closure: "
      "${_sentinel_root}:${_sentinel_path}")
  endif()
  if(_sentinel_kind STREQUAL "dir")
    if(NOT IS_DIRECTORY "${_sentinel_absolute}"
       OR IS_SYMLINK "${_sentinel_absolute}")
      message(FATAL_ERROR
        "stage_private_postgres: required runtime directory is missing: "
        "${_sentinel_absolute}")
    endif()
    set(_sentinel_sha_json "null")
  else()
    _require_regular_runtime_file(
      "${_sentinel_absolute}" "required PostgreSQL runtime sentinel")
    file(SHA256 "${_sentinel_absolute}" _sentinel_sha)
    set(_sentinel_sha_json "\"${_sentinel_sha}\"")
  endif()
  if(NOT "${_required_sentinels_json}" STREQUAL "")
    string(APPEND _required_sentinels_json ",\n")
  endif()
  string(APPEND _required_sentinels_json
    "      \"${_sentinel_name}\": {"
    "\"root\": \"${_sentinel_root}\", "
    "\"path\": \"${_sentinel_path}\", "
    "\"kind\": \"${_sentinel_kind}\", "
    "\"sha256\": ${_sentinel_sha_json}}")
endforeach()

# Seal the exact staged artifact set.
set(_artifact_hash_record "")
set(_sorted_extension_names ${_artifact_names})
list(SORT _sorted_extension_names)
foreach(_extension_name IN LISTS _sorted_extension_names)
  list(FIND _artifact_names "${_extension_name}" _artifact_index)
  list(GET
    _artifact_module_pathname_overrides
    ${_artifact_index}
    _module_pathname_override)
  if(_module_pathname_override STREQUAL "-")
    set(_module_pathname_override "")
  endif()
  string(APPEND _artifact_hash_record
    "artifact:${_extension_name}\n"
    "module_pathname_override:${_module_pathname_override}\n"
    "control:${_artifact_${_artifact_index}_control_filename}:"
    "${_artifact_${_artifact_index}_control_sha}\n"
    "install_sql:${_artifact_${_artifact_index}_install_sql_name}:"
    "${_artifact_${_artifact_index}_sql_sha}\n"
    "library:${_artifact_${_artifact_index}_library_filename}:"
    "${_artifact_${_artifact_index}_library_sha}\n")
endforeach()
string(SHA256 _aggregate_sha "${_artifact_hash_record}")

# Validate the copied server/tool suite is present. The runtime staged-vs-global
# resolution check is the integration probe that starts the
# copied server and runs CREATE EXTENSION; here we assert the tools exist and
# report their versions so a partial/mismatched copy fails the stage.
set(_tool_signatures "")
foreach(_tool ${_tools})
  set(_staged_tool "${STAGED_BINDIR}/${_tool}")
  _require_regular_runtime_file(
    "${_staged_tool}" "staged PostgreSQL tool ${_tool}")
  execute_process(
    COMMAND "${_staged_tool}" --version
    OUTPUT_VARIABLE _tool_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _tool_rc)
  if(NOT _tool_rc EQUAL 0)
    message(FATAL_ERROR "stage_private_postgres: staged ${_tool} --version failed")
  endif()
  _require_runtime_text("${_tool_version}" "staged tool version")
  file(SHA256 "${_staged_tool}" _tool_sha)
  _json_escape("${_tool_version}" _tool_version_json)
  if(NOT _tool_signatures STREQUAL "")
    string(APPEND _tool_signatures ",\n")
  endif()
  string(APPEND _tool_signatures
    "    {\"tool\": \"${_tool}\", \"staged_path\": \"${_bindir_rel}/${_tool}\", "
    "\"version\": \"${_tool_version_json}\", \"sha256\": \"${_tool_sha}\"}")
endforeach()

# Emit expected copied tool paths relative to STAGE_ROOT.
set(_tool_paths "")
foreach(_tool ${_tools})
  if(NOT _tool_paths STREQUAL "")
    string(APPEND _tool_paths ",\n")
  endif()
  string(APPEND _tool_paths
    "    \"${_tool}\": \"${_bindir_rel}/${_tool}\"")
endforeach()

_json_escape("${PG_VERSION}" _postgres_version_json)
_json_escape("${PG_CONFIGURE}" _postgres_configure_json)
_json_escape("${PG_BINDIR}" _postgres_bindir_json)
_json_escape("${PG_SHAREDIR}" _postgres_sharedir_json)
_json_escape("${PG_PKGLIBDIR}" _postgres_pkglibdir_json)
_json_escape("${_canonical_bindir}" _canonical_bindir_json)
_json_escape("${_canonical_sharedir}" _canonical_sharedir_json)
_json_escape("${_canonical_pkglibdir}" _canonical_pkglibdir_json)
_json_escape("${_bindir_rel}" _staged_bindir_json)
_json_escape("${_sharedir_rel}" _staged_sharedir_json)
_json_escape("${_pkglibdir_rel}" _staged_pkglibdir_json)

set(_artifact_manifest_entries "")
foreach(_artifact_index RANGE 0 ${_last_artifact_index})
  list(GET _artifact_names ${_artifact_index} _extension_name)
  list(GET _artifact_controls ${_artifact_index} _control_file)
  list(GET _artifact_install_sqls ${_artifact_index} _install_sql)
  list(GET _artifact_libraries ${_artifact_index} _library_file)
  list(GET
    _artifact_module_pathname_overrides
    ${_artifact_index}
    _module_pathname_override)
  if(_artifact_index GREATER 0)
    string(APPEND _artifact_manifest_entries ",\n")
  endif()
  if(_module_pathname_override STREQUAL "-")
    set(_module_pathname_json "null")
    get_filename_component(
      _effective_module_name
      "${_artifact_${_artifact_index}_library_filename}"
      NAME_WE)
  else()
    _json_escape("${_module_pathname_override}" _override_json)
    set(_module_pathname_json "\"${_override_json}\"")
    set(_effective_module_name "${_module_pathname_override}")
  endif()
  _json_escape("${_effective_module_name}" _effective_module_name_json)
  _json_escape("${_control_file}" _control_source_json)
  _json_escape("${_install_sql}" _install_sql_source_json)
  _json_escape("${_library_file}" _library_source_json)
  _json_escape(
    "${_artifact_${_artifact_index}_control_filename}"
    _control_filename_json)
  _json_escape(
    "${_artifact_${_artifact_index}_install_sql_name}"
    _install_sql_name_json)
  _json_escape(
    "${_artifact_${_artifact_index}_library_filename}"
    _library_filename_json)
  _json_escape(
    "${_artifact_${_artifact_index}_extension_version}"
    _extension_version_json)
  string(APPEND _artifact_manifest_entries
    "    {\n"
    "      \"name\": \"${_extension_name}\",\n"
    "      \"module_pathname_override\": ${_module_pathname_json},\n"
    "      \"effective_module_name\": "
    "\"${_effective_module_name_json}\",\n"
    "      \"control\": {"
    "\"source\": \"${_control_source_json}\", "
    "\"filename\": \"${_control_filename_json}\", "
    "\"staged\": \"${_staged_sharedir_json}/extension/"
    "${_control_filename_json}\", "
    "\"source_sha256\": "
    "\"${_artifact_${_artifact_index}_source_control_sha}\", "
    "\"sha256\": \"${_artifact_${_artifact_index}_control_sha}\"},\n"
    "      \"install_sql\": {"
    "\"source\": \"${_install_sql_source_json}\", "
    "\"filename\": \"${_install_sql_name_json}\", "
    "\"staged\": \"${_staged_sharedir_json}/extension/"
    "${_install_sql_name_json}\", "
    "\"version\": \"${_extension_version_json}\", "
    "\"sha256\": \"${_artifact_${_artifact_index}_sql_sha}\"},\n"
    "      \"library\": {"
    "\"source\": \"${_library_source_json}\", "
    "\"filename\": \"${_library_filename_json}\", "
    "\"staged\": \"${_staged_pkglibdir_json}/"
    "${_library_filename_json}\", "
    "\"sha256\": \"${_artifact_${_artifact_index}_library_sha}\"}\n"
    "    }")
endforeach()
string(CONCAT _artifact_manifest_block
  "  \"extension_artifacts\": {\n"
  "    \"aggregate_sha256\": \"${_aggregate_sha}\",\n"
  "    \"artifacts\": [\n"
  "${_artifact_manifest_entries}\n"
  "    ]\n"
  "  }")
string(CONCAT _runtime_manifest_block
  "  \"runtime_closure\": {\n"
  "    \"algorithm\": \"postgresql.materialized_package\",\n"
  "    \"schema_revision\": 1,\n"
  "    \"entry_count\": ${_runtime_entry_count},\n"
  "    \"aggregate_sha256\": \"${_runtime_aggregate_sha}\",\n"
  "    \"roots\": {\n"
  "      \"bindir\": {\"reported_source\": "
  "\"${_postgres_bindir_json}\", \"canonical_source\": "
  "\"${_canonical_bindir_json}\", \"staged\": "
  "\"${_staged_bindir_json}\"},\n"
  "      \"sharedir\": {\"reported_source\": "
  "\"${_postgres_sharedir_json}\", \"canonical_source\": "
  "\"${_canonical_sharedir_json}\", \"staged\": "
  "\"${_staged_sharedir_json}\"},\n"
  "      \"pkglibdir\": {\"reported_source\": "
  "\"${_postgres_pkglibdir_json}\", \"canonical_source\": "
  "\"${_canonical_pkglibdir_json}\", \"staged\": "
  "\"${_staged_pkglibdir_json}\"}\n"
  "    },\n"
  "    \"required_sentinels\": {\n"
  "${_required_sentinels_json}\n"
  "    },\n"
  "    \"loader_dependencies\": {\n"
  "      \"entry_count\": ${_loader_entry_count},\n"
  "      \"aggregate_sha256\": \"${_loader_aggregate_sha}\",\n"
  "      \"entries\": [\n"
  "${_loader_entries_json}\n"
  "      ]\n"
  "    },\n"
  "    \"entries\": [\n"
  "${_runtime_entries_json}\n"
  "    ]\n"
  "  },\n")
set(_manifest_schema "bioseq.private_postgres.stage_manifest")
set(_manifest_schema_revision 3)

string(CONCAT _manifest
"{
  \"schema\": \"${_manifest_schema}\",
  \"schema_revision\": ${_manifest_schema_revision},
  \"postgresql_version\": \"${_postgres_version_json}\",
  \"postgresql_configure\": \"${_postgres_configure_json}\",
  \"source\": {
    \"bindir\": \"${_postgres_bindir_json}\",
    \"sharedir\": \"${_postgres_sharedir_json}\",
    \"pkglibdir\": \"${_postgres_pkglibdir_json}\"
  },
  \"staged\": {
    \"bindir\": \"${_staged_bindir_json}\",
    \"sharedir\": \"${_staged_sharedir_json}\",
    \"pkglibdir\": \"${_staged_pkglibdir_json}\"
  },
${_runtime_manifest_block}
${_artifact_manifest_block},
  \"expected_tool_paths\": {
${_tool_paths}
  },
  \"tool_signatures\": [
${_tool_signatures}
  ]
}
")

get_filename_component(_manifest_dir "${MANIFEST_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_manifest_dir}")
get_filename_component(_hash_dir "${HASH_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_hash_dir}")
file(WRITE "${_manifest_temporary}" "${_manifest}")
file(WRITE "${_hash_temporary}" "${_aggregate_sha}\n")
file(RENAME
  "${_hash_temporary}" "${HASH_OUT}" RESULT _hash_publish_result)
if(NOT "${_hash_publish_result}" STREQUAL "0")
  file(REMOVE "${_manifest_temporary}" "${_hash_temporary}" "${HASH_OUT}")
  message(FATAL_ERROR
    "stage_private_postgres: failed to publish artifact hash: "
    "${_hash_publish_result}")
endif()
file(RENAME
  "${_manifest_temporary}" "${MANIFEST_OUT}" RESULT _manifest_publish_result)
if(NOT "${_manifest_publish_result}" STREQUAL "0")
  file(REMOVE "${_manifest_temporary}" "${MANIFEST_OUT}" "${HASH_OUT}")
  message(FATAL_ERROR
    "stage_private_postgres: failed to publish stage manifest: "
    "${_manifest_publish_result}")
endif()
message(STATUS "stage_private_postgres: staged ${PG_VERSION} under ${STAGE_ROOT}")
