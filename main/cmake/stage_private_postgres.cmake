# Relocatable private PostgreSQL extension stage (§13.2).
#
# Run with: cmake -DPG_CONFIG=... -DSTAGE_ROOT=... -DCONTROL_FILE=... \
#                 -DINSTALL_SQL=... -DLIBRARY_FILE=... -DEXTENSION_VERSION=... \
#                 -DMANIFEST_OUT=... -P stage_private_postgres.cmake
#
# It stages a relocatable copy of the selected PostgreSQL bindir/sharedir/
# pkglibdir under STAGE_ROOT, preserving each absolute path relative to a
# synthetic root so the copied server resolves the staged share/lib dirs; it
# excludes every globally installed Bioseq file, adds ONLY the current build
# target's control/install-SQL/DSO, and writes stage-manifest.json. There is no
# fallback that installs globally or bootstraps a fixed schema.

cmake_minimum_required(VERSION 3.24)

foreach(_required PG_CONFIG STAGE_ROOT CONTROL_FILE INSTALL_SQL LIBRARY_FILE
                  EXTENSION_VERSION MANIFEST_OUT)
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

# Fresh stage on every run — the prefix is an incremental build artifact.
file(REMOVE_RECURSE "${STAGE_ROOT}")

# Copy the built-in program/share/library trees so bundled languages and
# extensions remain available (§13.2 step 3).
file(COPY "${PG_BINDIR}/" DESTINATION "${STAGED_BINDIR}" USE_SOURCE_PERMISSIONS)
file(COPY "${PG_SHAREDIR}/" DESTINATION "${STAGED_SHAREDIR}" USE_SOURCE_PERMISSIONS)
file(COPY "${PG_PKGLIBDIR}/" DESTINATION "${STAGED_PKGLIBDIR}" USE_SOURCE_PERMISSIONS)

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
set(_tools postgres initdb pg_ctl createdb dropdb psql)
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
message(STATUS "stage_private_postgres: staged ${PG_VERSION} under ${STAGE_ROOT}")
