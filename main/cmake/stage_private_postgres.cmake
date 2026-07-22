# Relocatable private PostgreSQL extension stage (§13.2).
#
# Run with: cmake -DPG_CONFIG=... -DSTAGE_ROOT=... -DCONTROL_FILE=... \
#                 -DINSTALL_SQL=... -DLIBRARY_FILE=... -DEXTENSION_VERSION=... \
#                 -DMANIFEST_OUT=... -DHASH_OUT=... -P stage_private_postgres.cmake
#
# It stages a relocatable copy of the selected PostgreSQL bindir/sharedir/
# pkglibdir under STAGE_ROOT, preserving each absolute path relative to a
# synthetic root so the copied server resolves the staged share/lib dirs; it
# excludes every globally installed Bioseq file, adds ONLY the current build
# target's control/install-SQL/DSO, and writes stage-manifest.json. There is no
# fallback that installs globally or bootstraps a fixed schema.

cmake_minimum_required(VERSION 3.24)

foreach(_required PG_CONFIG STAGE_ROOT CONTROL_FILE INSTALL_SQL LIBRARY_FILE
                  EXTENSION_VERSION MANIFEST_OUT HASH_OUT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "stage_private_postgres: -D${_required} is required")
  endif()
endforeach()

foreach(_artifact CONTROL_FILE INSTALL_SQL LIBRARY_FILE)
  if(NOT EXISTS "${${_artifact}}")
    message(FATAL_ERROR
      "stage_private_postgres: ${_artifact} does not exist: ${${_artifact}}")
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
  MODULES "${LIBRARY_FILE}")
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

# Exclude every globally installed Bioseq file: sharedir/extension/bioseq*,
# pkglibdir/bioseq*, and the exact library filename (§13.2 step 3).
get_filename_component(_library_filename "${LIBRARY_FILE}" NAME)
get_filename_component(_control_filename "${CONTROL_FILE}" NAME)
# The extension version is the one baked into the install-SQL filename
# (bioseq--<version>.sql), not the website source version handed in.
get_filename_component(_install_sql_name "${INSTALL_SQL}" NAME)
string(REGEX REPLACE "^.*--(.+)\\.sql$" "\\1" _extension_version "${_install_sql_name}")
if(_extension_version STREQUAL "${_install_sql_name}")
  set(_extension_version "${EXTENSION_VERSION}")
endif()
file(GLOB _global_bioseq_share "${STAGED_SHAREDIR}/extension/bioseq*")
file(GLOB _global_bioseq_lib "${STAGED_PKGLIBDIR}/bioseq*")
if(_global_bioseq_share)
  file(REMOVE ${_global_bioseq_share})
endif()
if(_global_bioseq_lib)
  file(REMOVE ${_global_bioseq_lib})
endif()
if(EXISTS "${STAGED_PKGLIBDIR}/${_library_filename}")
  file(REMOVE "${STAGED_PKGLIBDIR}/${_library_filename}")
endif()

# Add ONLY the current build target's artifacts, never a globally installed
# Bioseq (§13.2 step 4).
file(MAKE_DIRECTORY "${STAGED_SHAREDIR}/extension")
file(COPY "${CONTROL_FILE}" DESTINATION "${STAGED_SHAREDIR}/extension")
file(COPY "${INSTALL_SQL}" DESTINATION "${STAGED_SHAREDIR}/extension")
file(COPY "${LIBRARY_FILE}" DESTINATION "${STAGED_PKGLIBDIR}")

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
  if(NOT _tool_signatures STREQUAL "")
    string(APPEND _tool_signatures ",\n")
  endif()
  string(APPEND _tool_signatures
    "    {\"tool\": \"${_tool}\", \"staged_path\": \"${_bindir_rel}/${_tool}\", "
    "\"version\": \"${_tool_version}\", \"sha256\": \"${_tool_sha}\"}")
endforeach()

# Hash the staged Bioseq artifacts (§13.2 step 5).
file(SHA256 "${CONTROL_FILE}" _control_sha)
file(SHA256 "${INSTALL_SQL}" _sql_sha)
file(SHA256 "${LIBRARY_FILE}" _library_sha)
string(CONCAT _artifact_hash_record
  "control:${_control_filename}:${_control_sha}\n"
  "install_sql:${_install_sql_name}:${_sql_sha}\n"
  "library:${_library_filename}:${_library_sha}\n")
string(SHA256 _aggregate_sha "${_artifact_hash_record}")

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

set(_manifest "{
  \"schema\": \"stage_manifest/v1\",
  \"postgresql_version\": \"${PG_VERSION}\",
  \"postgresql_configure\": \"${PG_CONFIGURE}\",
  \"source\": {
    \"bindir\": \"${PG_BINDIR}\",
    \"sharedir\": \"${PG_SHAREDIR}\",
    \"pkglibdir\": \"${PG_PKGLIBDIR}\"
  },
  \"staged\": {
    \"bindir\": \"${_bindir_rel}\",
    \"sharedir\": \"${_sharedir_rel}\",
    \"pkglibdir\": \"${_pkglibdir_rel}\"
  },
  \"bioseq_artifacts\": {
    \"aggregate_sha256\": \"${_aggregate_sha}\",
    \"control\": {\"staged\": \"${_sharedir_rel}/extension/${_control_filename}\", \"sha256\": \"${_control_sha}\"},
    \"install_sql\": {\"version\": \"${_extension_version}\", \"sha256\": \"${_sql_sha}\"},
    \"library\": {\"filename\": \"${_library_filename}\", \"staged\": \"${_pkglibdir_rel}/${_library_filename}\", \"sha256\": \"${_library_sha}\"}
  },
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
