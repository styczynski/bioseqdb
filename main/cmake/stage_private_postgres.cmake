# Relocatable private PostgreSQL extension stage (§13.2).
#
# Run with: cmake -DPG_CONFIG=... -DSTAGE_ROOT=... -DSTAGE_PARENT=... \
#                 -DCONTROL_FILE=... \
#                 -DINSTALL_SQL=... -DLIBRARY_FILE=... -DEXTENSION_VERSION=... \
#                 -DMANIFEST_OUT=... -DHASH_OUT=... -P stage_private_postgres.cmake
#
# Or pass one generated vector specification:
#
#   -DARTIFACT_SPEC=/absolute/path/extension-artifacts.json
#
# {
#   "schema": "bioseq_private_postgres_artifacts/v1",
#   "artifacts": [{
#     "name": "bioseq_cache",
#     "control": "/absolute/path/bioseq_cache.control",
#     "install_sql": "/absolute/path/bioseq_cache--1.0.sql",
#     "library": "/absolute/path/bioseq_cache.so",
#     "module_pathname_override": "bioseq_cache"
#   }]
# }
#
# It stages a relocatable copy of the selected PostgreSQL bindir/sharedir/
# pkglibdir under STAGE_ROOT, preserving each absolute path relative to a
# synthetic root so the copied server resolves the staged share/lib dirs; it
# excludes every globally installed Bioseq file, adds ONLY the current build
# targets' exact control/install-SQL/DSO vectors, and writes
# stage-manifest.json. There is no fallback that installs globally or
# bootstraps a fixed schema. The legacy single-artifact CLI and v1 manifest are
# retained for website consumers.

cmake_minimum_required(VERSION 3.24)

foreach(_required PG_CONFIG STAGE_ROOT STAGE_PARENT MANIFEST_OUT HASH_OUT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "stage_private_postgres: -D${_required} is required")
  endif()
endforeach()

set(_vector_mode FALSE)
if(DEFINED ARTIFACT_SPEC AND NOT "${ARTIFACT_SPEC}" STREQUAL "")
  set(_vector_mode TRUE)
  foreach(_legacy_arg IN ITEMS
      CONTROL_FILE INSTALL_SQL LIBRARY_FILE EXTENSION_VERSION)
    if(DEFINED ${_legacy_arg} AND NOT "${${_legacy_arg}}" STREQUAL "")
      message(FATAL_ERROR
        "stage_private_postgres: ARTIFACT_SPEC cannot be combined with "
        "-D${_legacy_arg}")
    endif()
  endforeach()
  if(NOT EXISTS "${ARTIFACT_SPEC}"
     OR IS_DIRECTORY "${ARTIFACT_SPEC}"
     OR IS_SYMLINK "${ARTIFACT_SPEC}")
    message(FATAL_ERROR
      "stage_private_postgres: ARTIFACT_SPEC must be a real file: "
      "${ARTIFACT_SPEC}")
  endif()
else()
  foreach(_required CONTROL_FILE INSTALL_SQL LIBRARY_FILE EXTENSION_VERSION)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "stage_private_postgres: -D${_required} is required without "
        "ARTIFACT_SPEC")
    endif()
  endforeach()
endif()

set(_artifact_names)
set(_artifact_controls)
set(_artifact_install_sqls)
set(_artifact_libraries)
set(_artifact_module_pathname_overrides)

if(_vector_mode)
  file(READ "${ARTIFACT_SPEC}" _artifact_spec_json)
  string(JSON _artifact_spec_schema
    ERROR_VARIABLE _artifact_spec_schema_error
    GET "${_artifact_spec_json}" schema)
  if(_artifact_spec_schema_error
     OR NOT _artifact_spec_schema STREQUAL
       "bioseq_private_postgres_artifacts/v1")
    message(FATAL_ERROR
      "stage_private_postgres: ARTIFACT_SPEC must use schema "
      "bioseq_private_postgres_artifacts/v1")
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

  string(JSON _top_level_member_count LENGTH "${_artifact_spec_json}")
  math(EXPR _top_level_last "${_top_level_member_count} - 1")
  foreach(_member_index RANGE 0 ${_top_level_last})
    string(JSON _member_name
      MEMBER "${_artifact_spec_json}" ${_member_index})
    if(NOT _member_name STREQUAL "schema"
       AND NOT _member_name STREQUAL "artifacts")
      message(FATAL_ERROR
        "stage_private_postgres: unknown ARTIFACT_SPEC member "
        "'${_member_name}'")
    endif()
  endforeach()

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
else()
  get_filename_component(_legacy_control_file "${CONTROL_FILE}" ABSOLUTE)
  get_filename_component(_legacy_install_sql "${INSTALL_SQL}" ABSOLUTE)
  get_filename_component(_legacy_library_file "${LIBRARY_FILE}" ABSOLUTE)
  get_filename_component(
    _legacy_control_filename "${_legacy_control_file}" NAME)
  if(NOT _legacy_control_filename MATCHES "^(.+)\\.control$")
    message(FATAL_ERROR
      "stage_private_postgres: CONTROL_FILE must have a .control basename")
  endif()
  set(_legacy_extension_name "${CMAKE_MATCH_1}")
  list(APPEND _artifact_names "${_legacy_extension_name}")
  list(APPEND _artifact_controls "${_legacy_control_file}")
  list(APPEND _artifact_install_sqls "${_legacy_install_sql}")
  list(APPEND _artifact_libraries "${_legacy_library_file}")
  list(APPEND _artifact_module_pathname_overrides "-")
  set(_artifact_count 1)
endif()

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
    if(_vector_mode AND IS_SYMLINK "${${_artifact_path}}")
      message(FATAL_ERROR
        "stage_private_postgres: ${_extension_name} vector artifact must "
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

# Query the selected pg_config for the layout and provenance (§13.2 step 1).
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
endforeach()

string(REGEX REPLACE "^/+" "" _bindir_rel "${PG_BINDIR}")

# Reproduce an absolute source path under STAGE_ROOT (§13.2 step 2).
function(_staged_path _absolute _out)
  string(REGEX REPLACE "^/+" "" _relative "${_absolute}")
  set(${_out} "${STAGE_ROOT}/${_relative}" PARENT_SCOPE)
endfunction()

_staged_path("${PG_BINDIR}" STAGED_BINDIR)
_staged_path("${PG_SHAREDIR}" STAGED_SHAREDIR)
_staged_path("${PG_PKGLIBDIR}" STAGED_PKGLIBDIR)

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
file(REMOVE_RECURSE "${STAGE_ROOT}")

# Copy the built-in program/share/library trees so bundled languages and
# extensions remain available (§13.2 step 3).
file(COPY "${PG_BINDIR}/" DESTINATION "${STAGED_BINDIR}" USE_SOURCE_PERMISSIONS)
file(COPY "${PG_SHAREDIR}/" DESTINATION "${STAGED_SHAREDIR}" USE_SOURCE_PERMISSIONS)
file(COPY "${PG_PKGLIBDIR}/" DESTINATION "${STAGED_PKGLIBDIR}" USE_SOURCE_PERMISSIONS)

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
    get_filename_component(_dependency_name "${_dependency}" NAME)
    set(_alias "${_root}/${_dependency_name}")
    if(EXISTS "${_alias}")
      file(REAL_PATH "${_alias}" _alias_real)
      if("${_alias_real}" STREQUAL "${_dependency_real}")
        set(_relative "${_dependency_name}")
      endif()
    endif()
  endif()
  set(${_out} "${_relative}" PARENT_SCOPE)
endfunction()

list(SORT _runtime_dependencies)
foreach(_dependency ${_runtime_dependencies})
  set(_runtime_relative "")
  set(_runtime_root "")
  foreach(_candidate_root ${_runtime_roots})
    _runtime_relative_path(
      "${_dependency}" "${_candidate_root}" _candidate_relative)
    if(NOT _candidate_relative STREQUAL "")
      set(_runtime_relative "${_candidate_relative}")
      set(_runtime_root "${_candidate_root}")
      break()
    endif()
  endforeach()
  if(_runtime_relative STREQUAL "")
    continue()
  endif()

  get_filename_component(_dependency_name "${_dependency}" NAME)
  if(_dependency_name MATCHES "^bioseq")
    message(FATAL_ERROR
      "stage_private_postgres: refusing globally installed Bioseq runtime dependency: ${_dependency}")
  endif()
  _staged_path("${_runtime_root}" _staged_runtime_root)
  set(_staged_dependency "${_staged_runtime_root}/${_runtime_relative}")
  if(EXISTS "${_staged_dependency}")
    continue()
  endif()
  get_filename_component(_staged_dependency_dir "${_staged_dependency}" DIRECTORY)
  file(MAKE_DIRECTORY "${_staged_dependency_dir}")
  file(COPY "${_dependency}"
       DESTINATION "${_staged_dependency_dir}"
       USE_SOURCE_PERMISSIONS FOLLOW_SYMLINK_CHAIN)
endforeach()

# Exclude every globally installed Bioseq file and every exact vector
# destination before publishing the current build's artifacts (§13.2 step 3).
file(GLOB _global_bioseq_share "${STAGED_SHAREDIR}/extension/bioseq*")
file(GLOB _global_bioseq_lib "${STAGED_PKGLIBDIR}/bioseq*")
if(_global_bioseq_share)
  file(REMOVE ${_global_bioseq_share})
endif()
if(_global_bioseq_lib)
  file(REMOVE ${_global_bioseq_lib})
endif()

# Add ONLY the current build targets' complete artifact vector, never a
# globally installed Bioseq (§13.2 step 4). An override changes only the
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

# Seal the exact staged vector (§13.2 step 5). Preserve the historical
# single-artifact hash record byte-for-byte in legacy mode.
if(_vector_mode)
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
else()
  string(CONCAT _artifact_hash_record
    "control:${_artifact_0_control_filename}:${_artifact_0_control_sha}\n"
    "install_sql:${_artifact_0_install_sql_name}:${_artifact_0_sql_sha}\n"
    "library:${_artifact_0_library_filename}:${_artifact_0_library_sha}\n")
endif()
string(SHA256 _aggregate_sha "${_artifact_hash_record}")

function(_json_escape _input _output)
  set(_escaped "${_input}")
  string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  string(REPLACE "\r" "\\r" _escaped "${_escaped}")
  string(REPLACE "\n" "\\n" _escaped "${_escaped}")
  string(REPLACE "\t" "\\t" _escaped "${_escaped}")
  set(${_output} "${_escaped}" PARENT_SCOPE)
endfunction()

# Validate the copied server/tool suite is present (§13.2 step 6). The runtime
# staged-vs-global resolution proof is the integration probe that starts the
# copied server and runs CREATE EXTENSION; here we assert the tools exist and
# report their versions so a partial/mismatched copy fails the stage.
set(_tool_signatures "")
foreach(_tool ${_tools})
  set(_staged_tool "${STAGED_BINDIR}/${_tool}")
  if(NOT EXISTS "${_staged_tool}")
    message(FATAL_ERROR "stage_private_postgres: staged ${_tool} missing")
  endif()
  execute_process(
    COMMAND "${_staged_tool}" --version
    OUTPUT_VARIABLE _tool_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _tool_rc)
  if(NOT _tool_rc EQUAL 0)
    message(FATAL_ERROR "stage_private_postgres: staged ${_tool} --version failed")
  endif()
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
string(REGEX REPLACE "^/+" "" _sharedir_rel "${PG_SHAREDIR}")
string(REGEX REPLACE "^/+" "" _pkglibdir_rel "${PG_PKGLIBDIR}")

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
_json_escape("${_bindir_rel}" _staged_bindir_json)
_json_escape("${_sharedir_rel}" _staged_sharedir_json)
_json_escape("${_pkglibdir_rel}" _staged_pkglibdir_json)

if(_vector_mode)
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
  set(_manifest_schema "stage_manifest/v2")
else()
  set(_manifest_schema "stage_manifest/v1")
  string(CONCAT _artifact_manifest_block
    "  \"bioseq_artifacts\": {\n"
    "    \"aggregate_sha256\": \"${_aggregate_sha}\",\n"
    "    \"control\": {\"staged\": "
    "\"${_staged_sharedir_json}/extension/"
    "${_artifact_0_control_filename}\", "
    "\"sha256\": \"${_artifact_0_control_sha}\"},\n"
    "    \"install_sql\": {\"version\": "
    "\"${_artifact_0_extension_version}\", "
    "\"sha256\": \"${_artifact_0_sql_sha}\"},\n"
    "    \"library\": {\"filename\": "
    "\"${_artifact_0_library_filename}\", "
    "\"staged\": \"${_staged_pkglibdir_json}/"
    "${_artifact_0_library_filename}\", "
    "\"sha256\": \"${_artifact_0_library_sha}\"}\n"
    "  }")
endif()

string(CONCAT _manifest
"{
  \"schema\": \"${_manifest_schema}\",
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
file(WRITE "${MANIFEST_OUT}" "${_manifest}")
get_filename_component(_hash_dir "${HASH_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_hash_dir}")
file(WRITE "${HASH_OUT}" "${_aggregate_sha}\n")
message(STATUS "stage_private_postgres: staged ${PG_VERSION} under ${STAGE_ROOT}")
