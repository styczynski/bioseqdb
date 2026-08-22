include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/bioseq_gcc_toolchain.cmake")

function(_bioseq_highway_gcc_canary_validate_host)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "HOST_SYSTEM;TARGET_SYSTEM;COMPILER_ID;GENERATOR;MAKE_PROGRAM"
    "")
  foreach(_required IN ITEMS
      HOST_SYSTEM TARGET_SYSTEM COMPILER_ID GENERATOR MAKE_PROGRAM)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "Highway GCC canary host validation requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "Highway GCC canary host validation received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ARG_HOST_SYSTEM STREQUAL "Darwin"
      OR NOT ARG_TARGET_SYSTEM STREQUAL "Darwin")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY is a local native-Darwin contract only; "
      "host=${ARG_HOST_SYSTEM}, target=${ARG_TARGET_SYSTEM}")
  endif()
  if(NOT ARG_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY requires the ordinary Clang host generator; "
      "found ${ARG_COMPILER_ID}")
  endif()
  if(NOT ARG_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY requires the Ninja host generator; found "
      "${ARG_GENERATOR}")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_MAKE_PROGRAM}"
      OR NOT EXISTS "${ARG_MAKE_PROGRAM}")
    message(FATAL_ERROR
      "BIOSEQ_HIGHWAY_GCC_CANARY requires an existing absolute Ninja path; "
      "found ${ARG_MAKE_PROGRAM}")
  endif()
endfunction()

function(_bioseq_highway_gcc_canary_require_homebrew_root root)
  file(REAL_PATH "${root}" _root_real)
  set(_homebrew_cellars)
  foreach(_prefix IN ITEMS "/opt/homebrew" "/usr/local")
    if(IS_DIRECTORY "${_prefix}/Cellar")
      file(REAL_PATH "${_prefix}/Cellar" _cellar_real)
      list(APPEND _homebrew_cellars "${_cellar_real}")
    endif()
  endforeach()
  foreach(_cellar IN LISTS _homebrew_cellars)
    string(FIND "${_root_real}/" "${_cellar}/" _under_cellar)
    if(_under_cellar EQUAL 0)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "BIOSEQ_HIGHWAY_GCC_CANARY requires a Homebrew GCC prefix under an "
    "authenticated Homebrew Cellar; selected ${_root_real}")
endfunction()

function(_bioseq_highway_gcc_canary_header_digest output include_root)
  if(NOT IS_ABSOLUTE "${include_root}"
      OR NOT EXISTS "${include_root}/hwy/highway.h"
      OR NOT EXISTS "${include_root}/hwy/targets.h")
    message(FATAL_ERROR
      "Highway GCC canary requires an absolute complete Highway include root: "
      "${include_root}")
  endif()
  file(REAL_PATH "${include_root}" _include_root)
  file(GLOB_RECURSE _headers LIST_DIRECTORIES FALSE
    "${_include_root}/hwy/*.h")
  list(SORT _headers)
  if(NOT _headers)
    message(FATAL_ERROR
      "Highway GCC canary found no headers beneath ${_include_root}/hwy")
  endif()
  set(_material
    "bioseq-highway-gcc-canary-headers\n"
    "revision=1\n")
  foreach(_header IN LISTS _headers)
    cmake_path(RELATIVE_PATH _header
      BASE_DIRECTORY "${_include_root}" OUTPUT_VARIABLE _relative)
    file(SHA256 "${_header}" _header_sha256)
    string(APPEND _material "${_relative}\t${_header_sha256}\n")
  endforeach()
  string(SHA256 _digest "${_material}")
  set(${output} "${_digest}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_query_driver
    output compiler description)
  if(NOT EXISTS "/usr/bin/env")
    message(FATAL_ERROR
      "Highway GCC canary requires /usr/bin/env for empty-environment queries")
  endif()
  execute_process(
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "${compiler}" ${ARGN}
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  string(STRIP "${_stdout}" _stdout)
  if(NOT _status EQUAL 0 OR "${_stdout}" STREQUAL "")
    message(FATAL_ERROR
      "Highway GCC canary could not query ${description}: "
      "status=${_status}, stderr=${_stderr}")
  endif()
  set(${output} "${_stdout}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_require_identity_value label value)
  if("${value}" STREQUAL "" OR "${value}" MATCHES "[;\t\r\n]")
    message(FATAL_ERROR
      "Highway GCC canary ${label} has an empty or unrepresentable identity: "
      "${value}")
  endif()
endfunction()

function(_bioseq_highway_gcc_canary_require_nonshared_writable_directory
    path label)
  if(NOT IS_ABSOLUTE "${path}" OR NOT IS_DIRECTORY "${path}")
    message(FATAL_ERROR
      "Highway GCC canary ${label} is not an existing absolute directory: "
      "${path}")
  endif()
  execute_process(
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "/usr/bin/stat" -f "%Sp" "${path}"
    RESULT_VARIABLE _stat_status
    OUTPUT_VARIABLE _permissions
    ERROR_VARIABLE _stat_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _stat_status EQUAL 0 OR NOT _permissions MATCHES "^d.........$")
    message(FATAL_ERROR
      "Highway GCC canary could not inspect ${label} permissions: "
      "status=${_stat_status}, output=${_permissions}, stderr=${_stat_stderr}")
  endif()
  string(SUBSTRING "${_permissions}" 5 1 _group_write)
  string(SUBSTRING "${_permissions}" 8 1 _world_write)
  if(_group_write STREQUAL "w" OR _world_write STREQUAL "w")
    message(FATAL_ERROR
      "Highway GCC canary ${label} is group/world writable: "
      "${path} (${_permissions})")
  endif()
endfunction()

function(_bioseq_highway_gcc_canary_apple_sdk_closure)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "CXX;DEVELOPER_DIR;SDK_PATH;SDK_ROOT;OUTPUT_VERSION;OUTPUT_BUILD_VERSION;OUTPUT_DIGEST"
    "")
  foreach(_required IN ITEMS
      CXX DEVELOPER_DIR SDK_PATH SDK_ROOT OUTPUT_VERSION
      OUTPUT_BUILD_VERSION OUTPUT_DIGEST)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK closure requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK closure received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  foreach(_identity IN ITEMS
      ARG_CXX ARG_DEVELOPER_DIR ARG_SDK_PATH ARG_SDK_ROOT)
    _bioseq_highway_gcc_canary_require_identity_value(
      "Apple SDK ${_identity}" "${${_identity}}")
  endforeach()
  if(NOT IS_ABSOLUTE "${ARG_CXX}"
      OR NOT EXISTS "${ARG_CXX}"
      OR IS_DIRECTORY "${ARG_CXX}")
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK probe requires an absolute compiler: "
      "${ARG_CXX}")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_DEVELOPER_DIR}"
      OR NOT IS_DIRECTORY "${ARG_DEVELOPER_DIR}")
    message(FATAL_ERROR
      "Highway GCC canary active Developer path is not an absolute directory: "
      "${ARG_DEVELOPER_DIR}")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_SDK_PATH}"
      OR NOT IS_DIRECTORY "${ARG_SDK_PATH}")
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK path is not an absolute directory: "
      "${ARG_SDK_PATH}")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_SDK_ROOT}"
      OR NOT IS_DIRECTORY "${ARG_SDK_ROOT}")
    message(FATAL_ERROR
      "Highway GCC canary sealed Apple SDK root is not an absolute directory: "
      "${ARG_SDK_ROOT}")
  endif()
  file(REAL_PATH "${ARG_DEVELOPER_DIR}" _developer_real)
  file(REAL_PATH "${ARG_SDK_PATH}" _sdk_real)
  file(REAL_PATH "${ARG_SDK_ROOT}" _expected_sdk_real)
  if(NOT _sdk_real STREQUAL _expected_sdk_real)
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK spelling no longer resolves to its sealed "
      "root: path=${ARG_SDK_PATH}, expected=${_expected_sdk_real}, "
      "observed=${_sdk_real}")
  endif()
  set(_sdk_parent
    "${_developer_real}/Platforms/MacOSX.platform/Developer/SDKs")
  if(NOT IS_DIRECTORY "${_sdk_parent}")
    message(FATAL_ERROR
      "Highway GCC canary active Developer directory owns no macOS SDK root: "
      "${_sdk_parent}")
  endif()
  file(REAL_PATH "${_sdk_parent}" _sdk_parent_real)
  string(FIND "${_sdk_real}/" "${_sdk_parent_real}/" _sdk_contained)
  if(NOT _sdk_contained EQUAL 0)
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK escaped the selected Developer "
      "directory: developer=${_developer_real}, sdk=${_sdk_real}")
  endif()
  # This is a deterministic qualification seal, not a hostile-local-writer
  # security boundary.  It rejects shared writable SDK trees and checkpoints
  # every compilation-relevant byte before/after each phase; a same-UID actor
  # racing and restoring inputs is explicitly outside the builder threat model.
  _bioseq_highway_gcc_canary_require_nonshared_writable_directory(
    "${_developer_real}" "active Developer directory")
  set(_sdk_ancestor "${_sdk_real}")
  while(TRUE)
    _bioseq_highway_gcc_canary_require_nonshared_writable_directory(
      "${_sdk_ancestor}" "active Apple SDK ancestor")
    if(_sdk_ancestor STREQUAL _developer_real)
      break()
    endif()
    get_filename_component(_sdk_ancestor "${_sdk_ancestor}" DIRECTORY)
    string(FIND "${_sdk_ancestor}/" "${_developer_real}/" _ancestor_contained)
    if(NOT _ancestor_contained EQUAL 0
        AND NOT _sdk_ancestor STREQUAL _developer_real)
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK ancestor escaped the selected "
        "Developer directory")
    endif()
  endwhile()

  set(_sdk_settings "${_sdk_real}/SDKSettings.plist")
  set(_sdk_settings_json "${_sdk_real}/SDKSettings.json")
  set(_system_version_plist
    "${_sdk_real}/System/Library/CoreServices/SystemVersion.plist")
  set(_libsystem_path "${_sdk_real}/usr/lib/libSystem.tbd")
  set(_required_regular_files
    "${_sdk_settings}" "${_sdk_settings_json}" "${_system_version_plist}")
  foreach(_required_file IN LISTS _required_regular_files)
    if(NOT EXISTS "${_required_file}" OR IS_DIRECTORY "${_required_file}")
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK closure is missing ${_required_file}")
    endif()
    if(IS_SYMLINK "${_required_file}")
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK metadata must be a regular in-tree "
        "file: ${_required_file}")
    endif()
    file(REAL_PATH "${_required_file}" _required_file_real)
    string(FIND "${_required_file_real}/" "${_sdk_real}/"
      _required_file_contained)
    if(NOT _required_file_contained EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK metadata escaped its sealed root: "
        "${_required_file}")
    endif()
  endforeach()
  foreach(_required_header IN ITEMS
      _bounds.h stdio.h stdlib.h stddef.h stdint.h sys/types.h)
    if(NOT EXISTS "${_sdk_real}/usr/include/${_required_header}"
        OR IS_DIRECTORY "${_sdk_real}/usr/include/${_required_header}")
      message(FATAL_ERROR
        "Highway GCC canary active Apple SDK is missing required C header "
        "usr/include/${_required_header}")
    endif()
  endforeach()
  if(NOT EXISTS "${_libsystem_path}" OR IS_DIRECTORY "${_libsystem_path}")
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK is missing usr/lib/libSystem.tbd")
  endif()

  set(_plist_query_labels
    sdk_version sdk_canonical_name system_version system_build_version)
  set(_plist_query_files
    "${_sdk_settings}" "${_sdk_settings}"
    "${_system_version_plist}" "${_system_version_plist}")
  set(_plist_query_keys
    Version CanonicalName ProductVersion ProductBuildVersion)
  foreach(_query_index RANGE 0 3)
    list(GET _plist_query_labels ${_query_index} _query_label)
    list(GET _plist_query_files ${_query_index} _query_file)
    list(GET _plist_query_keys ${_query_index} _query_key)
    execute_process(
      COMMAND "/usr/bin/env" -i
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
        "LC_ALL=C"
        "LANG=C"
        "/usr/bin/plutil" -extract "${_query_key}" raw -o - "${_query_file}"
      RESULT_VARIABLE _query_status
      OUTPUT_VARIABLE _query_output
      ERROR_VARIABLE _query_stderr
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _query_status EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary could not query ${_query_label}: "
        "status=${_query_status}, stderr=${_query_stderr}")
    endif()
    _bioseq_highway_gcc_canary_require_identity_value(
      "${_query_label}" "${_query_output}")
    set("_${_query_label}" "${_query_output}")
  endforeach()
  if(NOT _sdk_version MATCHES "^[0-9]+(\\.[0-9]+)+$")
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK has invalid version "
      "${_sdk_version}")
  endif()
  if(NOT _sdk_canonical_name STREQUAL "macosx${_sdk_version}")
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK canonical name/version disagree: "
      "${_sdk_canonical_name} versus ${_sdk_version}")
  endif()
  if(NOT _system_version STREQUAL _sdk_version)
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK and SystemVersion product versions "
      "disagree: ${_sdk_version} versus ${_system_version}")
  endif()

  file(GLOB_RECURSE _sdk_headers FOLLOW_SYMLINKS LIST_DIRECTORIES FALSE
    "${_sdk_real}/usr/include/*")
  file(GLOB_RECURSE _sdk_header_entries LIST_DIRECTORIES TRUE
    "${_sdk_real}/usr/include/*")
  list(SORT _sdk_headers)
  list(SORT _sdk_header_entries)
  if(NOT _sdk_headers)
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK has no usr/include closure")
  endif()
  set(_header_material
    "bioseq-highway-gcc-apple-sdk-c-headers\n"
    "revision=1\n")
  foreach(_header IN LISTS _sdk_headers)
    cmake_path(RELATIVE_PATH _header
      BASE_DIRECTORY "${_sdk_real}" OUTPUT_VARIABLE _header_relative)
    _bioseq_highway_gcc_canary_require_identity_value(
      "Apple SDK header path" "${_header_relative}")
    file(REAL_PATH "${_header}" _header_real)
    string(FIND "${_header_real}/" "${_sdk_real}/" _header_contained)
    if(NOT _header_contained EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK header escaped its sealed root: "
        "${_header}")
    endif()
    cmake_path(RELATIVE_PATH _header_real
      BASE_DIRECTORY "${_sdk_real}" OUTPUT_VARIABLE _header_real_relative)
    set(_header_link "regular")
    if(IS_SYMLINK "${_header}")
      file(READ_SYMLINK "${_header}" _header_link)
      _bioseq_highway_gcc_canary_require_identity_value(
        "Apple SDK header symlink" "${_header_link}")
    endif()
    file(SHA256 "${_header_real}" _header_sha256)
    string(APPEND _header_material
      "${_header_relative}\t${_header_link}\t${_header_real_relative}\t"
      "${_header_sha256}\n")
  endforeach()
  string(SHA256 _header_digest "${_header_material}")
  set(_header_symlink_material
    "bioseq-highway-gcc-apple-sdk-c-header-symlinks\n"
    "revision=1\n")
  foreach(_header_entry IN LISTS _sdk_header_entries)
    if(NOT IS_SYMLINK "${_header_entry}")
      continue()
    endif()
    cmake_path(RELATIVE_PATH _header_entry
      BASE_DIRECTORY "${_sdk_real}" OUTPUT_VARIABLE _entry_relative)
    file(READ_SYMLINK "${_header_entry}" _entry_link)
    _bioseq_highway_gcc_canary_require_identity_value(
      "Apple SDK header symlink path" "${_entry_relative}")
    _bioseq_highway_gcc_canary_require_identity_value(
      "Apple SDK header symlink target" "${_entry_link}")
    file(REAL_PATH "${_header_entry}" _entry_real)
    string(FIND "${_entry_real}/" "${_sdk_real}/" _entry_contained)
    if(NOT _entry_contained EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK header symlink escaped its sealed root: "
        "${_header_entry}")
    endif()
    cmake_path(RELATIVE_PATH _entry_real
      BASE_DIRECTORY "${_sdk_real}" OUTPUT_VARIABLE _entry_real_relative)
    string(APPEND _header_symlink_material
      "${_entry_relative}\t${_entry_link}\t${_entry_real_relative}\n")
  endforeach()
  string(SHA256 _header_symlink_digest "${_header_symlink_material}")

  file(REAL_PATH "${_libsystem_path}" _libsystem_real)
  string(FIND "${_libsystem_real}/" "${_sdk_real}/" _libsystem_contained)
  if(NOT _libsystem_contained EQUAL 0)
    message(FATAL_ERROR
      "Highway GCC canary libSystem.tbd escaped the active Apple SDK")
  endif()
  set(_libsystem_link "regular")
  if(IS_SYMLINK "${_libsystem_path}")
    file(READ_SYMLINK "${_libsystem_path}" _libsystem_link)
    _bioseq_highway_gcc_canary_require_identity_value(
      "Apple SDK libSystem symlink" "${_libsystem_link}")
  endif()
  cmake_path(RELATIVE_PATH _libsystem_real
    BASE_DIRECTORY "${_sdk_real}" OUTPUT_VARIABLE _libsystem_relative)
  file(SHA256 "${_libsystem_real}" _libsystem_sha256)
  file(SHA256 "${_sdk_settings}" _sdk_settings_sha256)
  file(SHA256 "${_sdk_settings_json}" _sdk_settings_json_sha256)
  file(SHA256 "${_system_version_plist}" _system_version_sha256)

  execute_process(
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "DEVELOPER_DIR=${_developer_real}"
      "SDKROOT=${_sdk_real}"
      "${ARG_CXX}"
      -std=gnu++26
      -isysroot "${_sdk_real}"
      -x c++
      -include cstdio
      -fsyntax-only /dev/null
    RESULT_VARIABLE _probe_status
    OUTPUT_VARIABLE _probe_stdout
    ERROR_VARIABLE _probe_stderr)
  if(NOT _probe_status EQUAL 0)
    message(FATAL_ERROR
      "Highway GCC canary authenticated compiler cannot use the active Apple "
      "SDK for <cstdio>: status=${_probe_status}, stderr=${_probe_stderr}")
  endif()

  string(CONCAT _sdk_material
    "bioseq-highway-gcc-apple-sdk\n"
    "revision=1\n"
    "developer=${_developer_real}\n"
    "sdk-path=${ARG_SDK_PATH}\n"
    "sdk-root=${_sdk_real}\n"
    "version=${_sdk_version}\n"
    "build=${_system_build_version}\n"
    "canonical-name=${_sdk_canonical_name}\n"
    "sdk-settings=${_sdk_settings_sha256}\n"
    "sdk-settings-json=${_sdk_settings_json_sha256}\n"
    "system-version=${_system_version_sha256}\n"
    "c-headers=${_header_digest}\n"
    "c-header-symlinks=${_header_symlink_digest}\n"
    "libsystem-link=${_libsystem_link}\n"
    "libsystem-target=${_libsystem_relative}\n"
    "libsystem=${_libsystem_sha256}\n")
  string(SHA256 _sdk_digest "${_sdk_material}")
  set(${ARG_OUTPUT_VERSION} "${_sdk_version}" PARENT_SCOPE)
  set(${ARG_OUTPUT_BUILD_VERSION} "${_system_build_version}" PARENT_SCOPE)
  set(${ARG_OUTPUT_DIGEST} "${_sdk_digest}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_select_apple_sdk)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "CXX;OUTPUT_DEVELOPER_DIR;OUTPUT_SDK_PATH;OUTPUT_SDK_ROOT;OUTPUT_VERSION;OUTPUT_BUILD_VERSION;OUTPUT_DIGEST"
    "")
  foreach(_required IN ITEMS
      CXX OUTPUT_DEVELOPER_DIR OUTPUT_SDK_PATH OUTPUT_SDK_ROOT OUTPUT_VERSION
      OUTPUT_BUILD_VERSION OUTPUT_DIGEST)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK selection requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK selection received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  # The host Clang build may intentionally use the standalone CLT SDK.  The
  # isolated Homebrew-GCC proof instead binds only the active Xcode selected by
  # xcode-select/xcrun; it never inherits CMAKE_OSX_SYSROOT, SDKROOT, or an
  # arbitrary installed-Xcode path from the outer process.
  foreach(_tool IN ITEMS env xcode-select xcrun plutil stat)
    set(_tool_path "/usr/bin/${_tool}")
    if(NOT EXISTS "${_tool_path}" OR IS_DIRECTORY "${_tool_path}")
      message(FATAL_ERROR
        "Highway GCC canary requires absolute Apple tool ${_tool_path}")
    endif()
  endforeach()
  set(_clean_environment
    "/usr/bin/env" -i
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
    "LC_ALL=C"
    "LANG=C")
  execute_process(
    COMMAND ${_clean_environment} "/usr/bin/xcode-select" -p
    RESULT_VARIABLE _developer_status
    OUTPUT_VARIABLE _developer_path
    ERROR_VARIABLE _developer_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  _bioseq_highway_gcc_canary_require_identity_value(
    "xcode-select Developer path" "${_developer_path}")
  if(NOT _developer_status EQUAL 0
      OR NOT IS_ABSOLUTE "${_developer_path}"
      OR NOT IS_DIRECTORY "${_developer_path}")
    message(FATAL_ERROR
      "Highway GCC canary could not resolve the active Developer directory: "
      "status=${_developer_status}, stderr=${_developer_stderr}")
  endif()
  file(REAL_PATH "${_developer_path}" _developer_real)
  set(_developer_environment
    ${_clean_environment} "DEVELOPER_DIR=${_developer_real}")
  set(_sdk_query_labels path version build_version)
  set(_sdk_query_arguments
    --show-sdk-path --show-sdk-version --show-sdk-build-version)
  foreach(_query_index RANGE 0 2)
    list(GET _sdk_query_labels ${_query_index} _query_label)
    list(GET _sdk_query_arguments ${_query_index} _query_argument)
    execute_process(
      COMMAND ${_developer_environment}
        "/usr/bin/xcrun" --sdk macosx "${_query_argument}"
      RESULT_VARIABLE _query_status
      OUTPUT_VARIABLE _query_output
      ERROR_VARIABLE _query_stderr
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _query_status EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary could not resolve active macOS SDK ${_query_label}: "
        "status=${_query_status}, stderr=${_query_stderr}")
    endif()
    _bioseq_highway_gcc_canary_require_identity_value(
      "xcrun macOS SDK ${_query_label}" "${_query_output}")
    set("_sdk_${_query_label}" "${_query_output}")
  endforeach()
  if(NOT IS_ABSOLUTE "${_sdk_path}" OR NOT IS_DIRECTORY "${_sdk_path}")
    message(FATAL_ERROR
      "Highway GCC canary xcrun returned a non-directory SDK path: "
      "${_sdk_path}")
  endif()
  file(REAL_PATH "${_sdk_path}" _sdk_real)
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${ARG_CXX}"
    DEVELOPER_DIR "${_developer_real}"
    SDK_PATH "${_sdk_path}"
    SDK_ROOT "${_sdk_real}"
    OUTPUT_VERSION _observed_version
    OUTPUT_BUILD_VERSION _observed_build_version
    OUTPUT_DIGEST _observed_digest)
  if(NOT _sdk_version STREQUAL _observed_version
      OR NOT _sdk_build_version STREQUAL _observed_build_version)
    message(FATAL_ERROR
      "Highway GCC canary xcrun and active SDK metadata disagree: "
      "xcrun=${_sdk_version}/${_sdk_build_version}, "
      "metadata=${_observed_version}/${_observed_build_version}")
  endif()
  set(${ARG_OUTPUT_DEVELOPER_DIR} "${_developer_real}" PARENT_SCOPE)
  set(${ARG_OUTPUT_SDK_PATH} "${_sdk_path}" PARENT_SCOPE)
  set(${ARG_OUTPUT_SDK_ROOT} "${_sdk_real}" PARENT_SCOPE)
  set(${ARG_OUTPUT_VERSION} "${_observed_version}" PARENT_SCOPE)
  set(${ARG_OUTPUT_BUILD_VERSION} "${_observed_build_version}" PARENT_SCOPE)
  set(${ARG_OUTPUT_DIGEST} "${_observed_digest}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_verify_apple_sdk)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "CXX;DEVELOPER_DIR;SDK_PATH;SDK_ROOT;VERSION;BUILD_VERSION;DIGEST"
    "")
  foreach(_required IN ITEMS
      CXX DEVELOPER_DIR SDK_PATH SDK_ROOT VERSION BUILD_VERSION DIGEST)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "Highway GCC canary Apple SDK verification requires ${_required}")
    endif()
  endforeach()
  _bioseq_highway_gcc_canary_select_apple_sdk(
    CXX "${ARG_CXX}"
    OUTPUT_DEVELOPER_DIR _observed_developer
    OUTPUT_SDK_PATH _observed_path
    OUTPUT_SDK_ROOT _observed_root
    OUTPUT_VERSION _observed_version
    OUTPUT_BUILD_VERSION _observed_build
    OUTPUT_DIGEST _observed_digest)
  if(NOT "${_observed_developer}" STREQUAL "${ARG_DEVELOPER_DIR}"
      OR NOT "${_observed_path}" STREQUAL "${ARG_SDK_PATH}"
      OR NOT "${_observed_root}" STREQUAL "${ARG_SDK_ROOT}"
      OR NOT "${_observed_version}" STREQUAL "${ARG_VERSION}"
      OR NOT "${_observed_build}" STREQUAL "${ARG_BUILD_VERSION}"
      OR NOT "${_observed_digest}" STREQUAL "${ARG_DIGEST}")
    message(FATAL_ERROR
      "Highway GCC canary active Apple SDK identity changed: expected="
      "${ARG_DEVELOPER_DIR}|${ARG_SDK_PATH}|${ARG_SDK_ROOT}|${ARG_VERSION}|"
      "${ARG_BUILD_VERSION}|${ARG_DIGEST}, observed=${_observed_developer}|"
      "${_observed_path}|${_observed_root}|${_observed_version}|"
      "${_observed_build}|${_observed_digest}")
  endif()
endfunction()

function(_bioseq_highway_gcc_canary_macho_records
    output_dependencies output_rpaths image)
  if(NOT IS_ABSOLUTE "${image}"
      OR NOT EXISTS "${image}"
      OR IS_DIRECTORY "${image}")
    message(FATAL_ERROR
      "Highway GCC canary cannot inspect non-regular Mach-O image ${image}")
  endif()
  if(NOT EXISTS "/usr/bin/env" OR NOT EXISTS "/usr/bin/otool")
    message(FATAL_ERROR
      "Highway GCC canary requires absolute env and otool executables")
  endif()
  execute_process(
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "/usr/bin/otool" -l "${image}"
    RESULT_VARIABLE _otool_status
    OUTPUT_VARIABLE _load_commands
    ERROR_VARIABLE _otool_stderr)
  if(NOT _otool_status EQUAL 0 OR "${_load_commands}" STREQUAL "")
    message(FATAL_ERROR
      "Highway GCC canary could not inspect Mach-O load commands for "
      "${image}: status=${_otool_status}, stderr=${_otool_stderr}")
  endif()
  string(FIND "${_load_commands}" ";" _semicolon_offset)
  if(NOT _semicolon_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary refuses an unrepresentable semicolon in Mach-O "
      "load-command output for ${image}")
  endif()

  string(REPLACE "\r\n" "\n" _load_commands "${_load_commands}")
  string(REPLACE "\n" ";" _load_command_lines "${_load_commands}")
  set(_dependencies)
  set(_rpaths)
  set(_load_header_count 0)
  set(_load_command_count 0)
  set(_expected_payload_count 0)
  set(_parsed_payload_count 0)
  set(_in_load_command FALSE)
  set(_current_cmd_seen FALSE)
  set(_pending_payload "")
  set(_pending_cmd "")
  foreach(_raw_line IN LISTS _load_command_lines)
    string(REGEX REPLACE "\r$" "" _raw_line "${_raw_line}")
    string(STRIP "${_raw_line}" _line)
    if(_line MATCHES "^Load command [0-9]+$")
      if(NOT "${_pending_payload}" STREQUAL "")
        message(FATAL_ERROR
          "Highway GCC canary found unparsed ${_pending_cmd} record in "
          "${image}")
      endif()
      if(_in_load_command AND NOT _current_cmd_seen)
        message(FATAL_ERROR
          "Highway GCC canary found a load-command record without an exact "
          "cmd field in ${image}")
      endif()
      set(_in_load_command TRUE)
      set(_current_cmd_seen FALSE)
      math(EXPR _load_header_count "${_load_header_count} + 1")
      continue()
    endif()
    if(_line MATCHES "^cmd (LC_[A-Z0-9_]+)$")
      if(NOT _in_load_command OR _current_cmd_seen)
        message(FATAL_ERROR
          "Highway GCC canary found an unaccounted Mach-O cmd record in "
          "${image}: ${_line}")
      endif()
      if(NOT "${_pending_payload}" STREQUAL "")
        message(FATAL_ERROR
          "Highway GCC canary found unparsed ${_pending_cmd} record in "
          "${image}")
      endif()
      set(_current_cmd_seen TRUE)
      math(EXPR _load_command_count "${_load_command_count} + 1")
      set(_cmd "${CMAKE_MATCH_1}")
      if(_cmd STREQUAL "LC_LOAD_DYLIB"
          OR _cmd STREQUAL "LC_LOAD_WEAK_DYLIB"
          OR _cmd STREQUAL "LC_REEXPORT_DYLIB"
          OR _cmd STREQUAL "LC_LOAD_UPWARD_DYLIB"
          OR _cmd STREQUAL "LC_LAZY_LOAD_DYLIB"
          OR _cmd STREQUAL "LC_LOAD_DYLINKER")
        set(_pending_payload "dependency")
        set(_pending_cmd "${_cmd}")
        math(EXPR _expected_payload_count "${_expected_payload_count} + 1")
      elseif(_cmd STREQUAL "LC_ID_DYLIB")
        set(_pending_payload "identity")
        set(_pending_cmd "${_cmd}")
        math(EXPR _expected_payload_count "${_expected_payload_count} + 1")
      elseif(_cmd STREQUAL "LC_RPATH")
        set(_pending_payload "rpath")
        set(_pending_cmd "${_cmd}")
        math(EXPR _expected_payload_count "${_expected_payload_count} + 1")
      elseif(_cmd STREQUAL "LC_DYLD_ENVIRONMENT")
        message(FATAL_ERROR
          "Highway GCC canary refuses LC_DYLD_ENVIRONMENT in ${image}")
      elseif(_cmd MATCHES "^LC_LOAD" OR _cmd MATCHES "DYLIB$")
        message(FATAL_ERROR
          "Highway GCC canary cannot exact-account unsupported dependency "
          "command ${_cmd} in ${image}")
      endif()
      continue()
    endif()

    if(_pending_payload STREQUAL "rpath")
      if(_raw_line MATCHES
          "^[ \t]*path (.+) \\(offset [0-9]+\\)[ \t]*$")
        set(_payload_value "${CMAKE_MATCH_1}")
        if("${_payload_value}" STREQUAL "")
          message(FATAL_ERROR
            "Highway GCC canary found an empty ${_pending_cmd} path in "
            "${image}")
        endif()
        list(APPEND _rpaths "${_payload_value}")
        set(_pending_payload "")
        set(_pending_cmd "")
        math(EXPR _parsed_payload_count "${_parsed_payload_count} + 1")
      endif()
    elseif(_pending_payload STREQUAL "dependency"
        OR _pending_payload STREQUAL "identity")
      if(_raw_line MATCHES
          "^[ \t]*name (.+) \\(offset [0-9]+\\)[ \t]*$")
        set(_payload_value "${CMAKE_MATCH_1}")
        if("${_payload_value}" STREQUAL "")
          message(FATAL_ERROR
            "Highway GCC canary found an empty ${_pending_cmd} name in "
            "${image}")
        endif()
        if(_pending_payload STREQUAL "dependency")
          list(APPEND _dependencies "${_payload_value}")
        endif()
        set(_pending_payload "")
        set(_pending_cmd "")
        math(EXPR _parsed_payload_count "${_parsed_payload_count} + 1")
      endif()
    endif()
  endforeach()
  if(NOT "${_pending_payload}" STREQUAL "")
    message(FATAL_ERROR
      "Highway GCC canary found unparsed ${_pending_cmd} record in ${image}")
  endif()
  if(_in_load_command AND NOT _current_cmd_seen)
    message(FATAL_ERROR
      "Highway GCC canary found a trailing load-command record without an "
      "exact cmd field in ${image}")
  endif()
  if(_load_header_count EQUAL 0
      OR NOT _load_header_count EQUAL _load_command_count
      OR NOT _expected_payload_count EQUAL _parsed_payload_count)
    message(FATAL_ERROR
      "Highway GCC canary did not exact-account every Mach-O load command "
      "in ${image}: headers=${_load_header_count}, commands="
      "${_load_command_count}, expected payloads=${_expected_payload_count}, "
      "parsed payloads=${_parsed_payload_count}")
  endif()
  list(REMOVE_DUPLICATES _dependencies)
  list(REMOVE_DUPLICATES _rpaths)
  set(${output_dependencies} "${_dependencies}" PARENT_SCOPE)
  set(${output_rpaths} "${_rpaths}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_expand_macho_rpaths
    output loader executable)
  file(REAL_PATH "${loader}" _loader_real)
  file(REAL_PATH "${executable}" _executable_real)
  get_filename_component(_loader_dir "${_loader_real}" DIRECTORY)
  get_filename_component(_executable_dir "${_executable_real}" DIRECTORY)
  set(_expanded_rpaths)
  foreach(_rpath IN LISTS ARGN)
    if(IS_ABSOLUTE "${_rpath}")
      set(_expanded_rpath "${_rpath}")
    elseif(_rpath STREQUAL "@loader_path")
      set(_expanded_rpath "${_loader_dir}")
    elseif(_rpath MATCHES "^@loader_path/(.*)$")
      set(_expanded_rpath "${_loader_dir}/${CMAKE_MATCH_1}")
    elseif(_rpath STREQUAL "@executable_path")
      set(_expanded_rpath "${_executable_dir}")
    elseif(_rpath MATCHES "^@executable_path/(.*)$")
      set(_expanded_rpath "${_executable_dir}/${CMAKE_MATCH_1}")
    else()
      message(FATAL_ERROR
        "Highway GCC canary refuses unsupported or nested Mach-O rpath "
        "${_rpath} in ${loader}")
    endif()
    cmake_path(NORMAL_PATH _expanded_rpath OUTPUT_VARIABLE _expanded_rpath)
    list(APPEND _expanded_rpaths "${_expanded_rpath}")
  endforeach()
  list(REMOVE_DUPLICATES _expanded_rpaths)
  set(${output} "${_expanded_rpaths}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_resolve_macho_dependency
    output dependency loader executable)
  file(REAL_PATH "${loader}" _loader_real)
  file(REAL_PATH "${executable}" _executable_real)
  get_filename_component(_loader_dir "${_loader_real}" DIRECTORY)
  get_filename_component(_executable_dir "${_executable_real}" DIRECTORY)
  set(_candidates)
  if(IS_ABSOLUTE "${dependency}")
    list(APPEND _candidates "${dependency}")
  elseif(dependency MATCHES "^@loader_path/(.+)$")
    list(APPEND _candidates "${_loader_dir}/${CMAKE_MATCH_1}")
  elseif(dependency MATCHES "^@executable_path/(.+)$")
    list(APPEND _candidates "${_executable_dir}/${CMAKE_MATCH_1}")
  elseif(dependency MATCHES "^@rpath/(.+)$")
    set(_suffix "${CMAKE_MATCH_1}")
    foreach(_rpath IN LISTS ARGN)
      if(NOT IS_ABSOLUTE "${_rpath}")
        message(FATAL_ERROR
          "Highway GCC canary received an unexpanded rpath ${_rpath} while "
          "resolving ${dependency}")
      endif()
      list(APPEND _candidates "${_rpath}/${_suffix}")
    endforeach()
  else()
    message(FATAL_ERROR
      "Highway GCC canary refuses unsupported Mach-O dependency spelling "
      "${dependency} in ${loader}")
  endif()
  foreach(_candidate IN LISTS _candidates)
    cmake_path(NORMAL_PATH _candidate OUTPUT_VARIABLE _candidate_normal)
    if(EXISTS "${_candidate_normal}" AND NOT IS_DIRECTORY "${_candidate_normal}")
      file(REAL_PATH "${_candidate_normal}" _candidate_real)
      set(${output} "${_candidate_real}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "Highway GCC canary could not resolve Mach-O dependency ${dependency} "
    "from ${loader}; authenticated rpaths=${ARGN}")
endfunction()

function(_bioseq_highway_gcc_canary_collect_macho_closure
    output_rows output_paths executable)
  if(NOT IS_ABSOLUTE "${executable}"
      OR NOT EXISTS "${executable}"
      OR IS_DIRECTORY "${executable}")
    message(FATAL_ERROR
      "Highway GCC canary cannot close non-regular Mach-O image ${executable}")
  endif()
  file(REAL_PATH "${executable}" _executable_real)
  set(_queue "${_executable_real}")
  string(SHA256 _root_key "${_executable_real}")
  set("_ancestor_rpaths_${_root_key}" "")
  set(_visited)
  set(_closure_rows)
  set(_closure_paths)
  while(_queue)
    list(POP_FRONT _queue _image)
    list(FIND _visited "${_image}" _visited_offset)
    if(NOT _visited_offset EQUAL -1)
      continue()
    endif()
    list(APPEND _visited "${_image}")
    string(SHA256 _image_key "${_image}")
    set(_ancestor_rpaths "${_ancestor_rpaths_${_image_key}}")
    _bioseq_highway_gcc_canary_macho_records(
      _dependencies _raw_rpaths "${_image}")
    _bioseq_highway_gcc_canary_expand_macho_rpaths(
      _current_rpaths "${_image}" "${_executable_real}" ${_raw_rpaths})
    set(_search_rpaths ${_current_rpaths} ${_ancestor_rpaths})
    list(REMOVE_DUPLICATES _search_rpaths)
    # Prepending a reversed dependency list gives depth-first dyld load order.
    # If a later root record already queued the same image, the earlier
    # dependency chain wins and supplies the correct inherited rpath stack.
    list(REVERSE _dependencies)
    foreach(_dependency IN LISTS _dependencies)
      if(_dependency MATCHES "libc\\+\\+"
          OR _dependency MATCHES "(^|/)libhwy([^/]*)$")
        message(FATAL_ERROR
          "Highway GCC canary found forbidden mixed runtime dependency "
          "${_dependency} in ${_image}")
      endif()
      if(IS_ABSOLUTE "${_dependency}")
        cmake_path(
          NORMAL_PATH _dependency OUTPUT_VARIABLE _dependency_normal)
        if(_dependency_normal MATCHES "^/usr/lib(/|$)"
            OR _dependency_normal MATCHES "^/System/Library(/|$)")
          continue()
        endif()
      endif()
      _bioseq_highway_gcc_canary_resolve_macho_dependency(
        _resolved "${_dependency}" "${_image}" "${_executable_real}"
        ${_search_rpaths})
      if(_resolved MATCHES "libc\\+\\+"
          OR _resolved MATCHES "(^|/)libhwy([^/]*)$")
        message(FATAL_ERROR
          "Highway GCC canary resolved forbidden mixed runtime dependency "
          "${_dependency} to ${_resolved}")
      endif()
      if(_resolved MATCHES "^/usr/lib(/|$)"
          OR _resolved MATCHES "^/System/Library(/|$)")
        continue()
      endif()
      file(SHA256 "${_resolved}" _resolved_sha256)
      list(APPEND _closure_paths "${_resolved}")
      list(APPEND _closure_rows "${_resolved}\t${_resolved_sha256}")
      list(FIND _visited "${_resolved}" _resolved_visited_offset)
      if(_resolved_visited_offset EQUAL -1)
        string(SHA256 _resolved_key "${_resolved}")
        set("_ancestor_rpaths_${_resolved_key}" "${_search_rpaths}")
        list(REMOVE_ITEM _queue "${_resolved}")
        list(PREPEND _queue "${_resolved}")
      endif()
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES _closure_paths)
  list(REMOVE_DUPLICATES _closure_rows)
  list(SORT _closure_paths)
  list(SORT _closure_rows)
  set(${output_rows} "${_closure_rows}" PARENT_SCOPE)
  set(${output_paths} "${_closure_paths}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_gcc_canary_driver_closure
    output_digest output_libstdcxx output_libgcc root compiler)
  file(REAL_PATH "${root}" _root_real)
  file(REAL_PATH "${compiler}" _compiler_real)
  string(FIND "${_compiler_real}/" "${_root_real}/" _compiler_owned)
  if(NOT _compiler_owned EQUAL 0)
    message(FATAL_ERROR
      "Highway GCC canary compiler escaped its authenticated prefix")
  endif()

  _bioseq_highway_gcc_canary_query_driver(
    _cc1plus "${_compiler_real}" "cc1plus" -print-prog-name=cc1plus)
  _bioseq_highway_gcc_canary_query_driver(
    _collect2 "${_compiler_real}" "collect2" -print-prog-name=collect2)
  _bioseq_highway_gcc_canary_query_driver(
    _libstdcxx "${_compiler_real}" "libstdc++.dylib"
    -print-file-name=libstdc++.dylib)
  _bioseq_highway_gcc_canary_query_driver(
    _libstdcxx_static "${_compiler_real}" "libstdc++.a"
    -print-file-name=libstdc++.a)
  _bioseq_highway_gcc_canary_query_driver(
    _libgcc "${_compiler_real}" "libgcc_s.1.1.dylib"
    -print-file-name=libgcc_s.1.1.dylib)
  _bioseq_highway_gcc_canary_query_driver(
    _libgcc_static "${_compiler_real}" "libgcc.a"
    -print-file-name=libgcc.a)
  _bioseq_highway_gcc_canary_query_driver(
    _libgcc_eh "${_compiler_real}" "libgcc_eh.a"
    -print-file-name=libgcc_eh.a)
  _bioseq_highway_gcc_canary_query_driver(
    _specs "${_compiler_real}" "effective GCC specs" -dumpspecs)
  _bioseq_highway_gcc_canary_query_driver(
    _search_dirs "${_compiler_real}" "effective GCC search directories"
    -print-search-dirs)
  _bioseq_highway_gcc_canary_query_driver(
    _internal_include "${_compiler_real}" "GCC internal include directory"
    -print-file-name=include)
  _bioseq_highway_gcc_canary_query_driver(
    _internal_include_fixed "${_compiler_real}"
    "GCC fixed internal include directory" -print-file-name=include-fixed)
  string(SHA256 _specs_sha256 "${_specs}")
  string(SHA256 _search_dirs_sha256 "${_search_dirs}")

  set(_artifact_labels
    compiler cc1plus collect2 libstdcxx_shared libstdcxx_static
    libgcc_shared libgcc_static libgcc_eh)
  set(_artifact_paths
    "${_compiler_real}" "${_cc1plus}" "${_collect2}" "${_libstdcxx}"
    "${_libstdcxx_static}" "${_libgcc}" "${_libgcc_static}"
    "${_libgcc_eh}")
  list(LENGTH _artifact_labels _artifact_count)
  math(EXPR _artifact_last "${_artifact_count} - 1")
  set(_artifact_rows)
  foreach(_artifact_index RANGE 0 ${_artifact_last})
    list(GET _artifact_labels ${_artifact_index} _artifact_label)
    list(GET _artifact_paths ${_artifact_index} _artifact_path)
    if(NOT IS_ABSOLUTE "${_artifact_path}"
        OR NOT EXISTS "${_artifact_path}"
        OR IS_DIRECTORY "${_artifact_path}")
      message(FATAL_ERROR
        "Highway GCC canary ${_artifact_label} is not an absolute regular "
        "driver artifact: ${_artifact_path}")
    endif()
    file(REAL_PATH "${_artifact_path}" _artifact_real)
    string(FIND "${_artifact_real}/" "${_root_real}/" _artifact_owned)
    if(NOT _artifact_owned EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary ${_artifact_label} escaped the selected prefix: "
        "${_artifact_real}")
    endif()
    file(SHA256 "${_artifact_real}" _artifact_sha256)
    list(APPEND _artifact_rows
      "${_artifact_label}\t${_artifact_real}\t${_artifact_sha256}")
    if(_artifact_label STREQUAL "libstdcxx_shared")
      set(_libstdcxx_real "${_artifact_real}")
    elseif(_artifact_label STREQUAL "libgcc_shared")
      set(_libgcc_real "${_artifact_real}")
    endif()
  endforeach()

  # Seal every compiler-owned textual header tree that participates in this
  # translation, including arm_neon.h and the GCC-provided builtin headers.
  set(_header_tree_labels libstdcxx gcc-internal gcc-include-fixed)
  set(_header_tree_paths
    "${_root_real}/include/c++" "${_internal_include}"
    "${_internal_include_fixed}")
  set(_header_rows)
  list(LENGTH _header_tree_labels _header_tree_count)
  math(EXPR _header_tree_last "${_header_tree_count} - 1")
  foreach(_header_tree_index RANGE 0 ${_header_tree_last})
    list(GET _header_tree_labels ${_header_tree_index} _header_tree_label)
    list(GET _header_tree_paths ${_header_tree_index} _header_tree)
    if(NOT IS_ABSOLUTE "${_header_tree}"
        OR NOT IS_DIRECTORY "${_header_tree}")
      message(FATAL_ERROR
        "Highway GCC canary ${_header_tree_label} header tree is not an "
        "absolute directory: ${_header_tree}")
    endif()
    file(REAL_PATH "${_header_tree}" _header_tree_real)
    string(FIND "${_header_tree_real}/" "${_root_real}/" _tree_owned)
    if(NOT _tree_owned EQUAL 0)
      message(FATAL_ERROR
        "Highway GCC canary ${_header_tree_label} header tree escaped the "
        "selected prefix: ${_header_tree_real}")
    endif()
    file(GLOB_RECURSE _tree_headers FOLLOW_SYMLINKS LIST_DIRECTORIES FALSE
      "${_header_tree_real}/*")
    list(SORT _tree_headers)
    if(NOT _tree_headers)
      message(FATAL_ERROR
        "Highway GCC canary found no ${_header_tree_label} textual headers")
    endif()
    set(_tree_material
      "bioseq-gcc-header-tree\n"
      "revision=2\n"
      "${_header_tree_label}\n")
    foreach(_header IN LISTS _tree_headers)
      file(REAL_PATH "${_header}" _header_real)
      string(FIND "${_header_real}/" "${_root_real}/" _header_owned)
      if(NOT _header_owned EQUAL 0)
        message(FATAL_ERROR
          "Highway GCC canary ${_header_tree_label} header escaped the "
          "selected prefix: ${_header_real}")
      endif()
      cmake_path(RELATIVE_PATH _header
        BASE_DIRECTORY "${_root_real}" OUTPUT_VARIABLE _header_relative)
      file(SHA256 "${_header_real}" _header_sha256)
      string(APPEND _tree_material
        "${_header_relative}\t${_header_sha256}\n")
    endforeach()
    string(SHA256 _tree_sha256 "${_tree_material}")
    list(APPEND _header_rows
      "${_header_tree_label}\t${_header_tree_real}\t${_tree_sha256}")
  endforeach()

  # The frontend and linker helpers are executable code in the effective
  # compiler. Authenticate every non-system dylib they can load, recursively.
  set(_driver_dependency_rows)
  foreach(_driver_image IN ITEMS "${_compiler_real}" "${_cc1plus}" "${_collect2}")
    _bioseq_highway_gcc_canary_collect_macho_closure(
      _image_dependency_rows _image_dependency_paths "${_driver_image}")
    list(APPEND _driver_dependency_rows ${_image_dependency_rows})
  endforeach()
  list(REMOVE_DUPLICATES _driver_dependency_rows)
  list(SORT _driver_dependency_rows)

  list(SORT _artifact_rows)
  list(SORT _header_rows)
  string(CONCAT _closure_material
    "bioseq-highway-gcc-driver-closure\n"
    "revision=2\n"
    "specs\t${_specs_sha256}\n"
    "search-dirs\t${_search_dirs_sha256}\n")
  foreach(_artifact_row IN LISTS _artifact_rows)
    string(APPEND _closure_material "artifact\t${_artifact_row}\n")
  endforeach()
  foreach(_header_row IN LISTS _header_rows)
    string(APPEND _closure_material "headers\t${_header_row}\n")
  endforeach()
  foreach(_dependency_row IN LISTS _driver_dependency_rows)
    string(APPEND _closure_material "tool-dylib\t${_dependency_row}\n")
  endforeach()
  string(SHA256 _closure_sha256 "${_closure_material}")
  set(${output_digest} "${_closure_sha256}" PARENT_SCOPE)
  set(${output_libstdcxx} "${_libstdcxx_real}" PARENT_SCOPE)
  set(${output_libgcc} "${_libgcc_real}" PARENT_SCOPE)
endfunction()

function(bioseq_add_highway_gcc_canary)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "TARGET;SOURCE_ROOT;HIGHWAY_INCLUDE_ROOT;GENERATION_TARGET;GENERATED_ROOT;PRIMARY;DRIVER;MAIN"
    "")
  foreach(_required IN ITEMS
      TARGET SOURCE_ROOT HIGHWAY_INCLUDE_ROOT GENERATION_TARGET GENERATED_ROOT
      PRIMARY DRIVER MAIN)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "bioseq_add_highway_gcc_canary requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "bioseq_add_highway_gcc_canary received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(TARGET "${ARG_TARGET}")
    message(FATAL_ERROR
      "Highway GCC canary target already exists: ${ARG_TARGET}")
  endif()
  if(NOT TARGET "${ARG_GENERATION_TARGET}")
    message(FATAL_ERROR
      "Highway GCC canary generation target does not exist: "
      "${ARG_GENERATION_TARGET}")
  endif()
  foreach(_checked_source IN ITEMS PRIMARY DRIVER MAIN)
    if(NOT IS_ABSOLUTE "${ARG_${_checked_source}}"
        OR NOT EXISTS "${ARG_${_checked_source}}"
        OR IS_DIRECTORY "${ARG_${_checked_source}}")
      message(FATAL_ERROR
        "Highway GCC canary ${_checked_source} must be an existing absolute "
        "regular path: ${ARG_${_checked_source}}")
    endif()
  endforeach()
  if(NOT IS_ABSOLUTE "${ARG_GENERATED_ROOT}")
    message(FATAL_ERROR
      "Highway GCC canary GENERATED_ROOT must be absolute")
  endif()
  if(NOT IS_ABSOLUTE "${ARG_SOURCE_ROOT}"
      OR NOT EXISTS "${ARG_SOURCE_ROOT}/contract.cppm"
      OR NOT EXISTS "${ARG_SOURCE_ROOT}/target_primary.cppm.in")
    message(FATAL_ERROR
      "Highway GCC canary SOURCE_ROOT must own the complete source/template "
      "closure: ${ARG_SOURCE_ROOT}")
  endif()
  file(REAL_PATH "${ARG_SOURCE_ROOT}" _canary_source_root)
  file(REAL_PATH "${ARG_HIGHWAY_INCLUDE_ROOT}" _highway_include_root)
  _bioseq_highway_gcc_canary_header_digest(
    _highway_header_digest "${_highway_include_root}")

  _bioseq_highway_gcc_canary_validate_host(
    HOST_SYSTEM "${CMAKE_HOST_SYSTEM_NAME}"
    TARGET_SYSTEM "${CMAKE_SYSTEM_NAME}"
    COMPILER_ID "${CMAKE_CXX_COMPILER_ID}"
    GENERATOR "${CMAKE_GENERATOR}"
    MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")

  set(_requested_root "")
  if(DEFINED ENV{BIOSEQ_GCC16_ROOT})
    set(_requested_root "$ENV{BIOSEQ_GCC16_ROOT}")
  elseif(BIOSEQ_GCC16_ROOT)
    set(_requested_root "${BIOSEQ_GCC16_ROOT}")
  endif()
  set(_requested_modules "")
  if(DEFINED ENV{BIOSEQ_LIBSTDCXX_MODULES_JSON})
    set(_requested_modules "$ENV{BIOSEQ_LIBSTDCXX_MODULES_JSON}")
  elseif(BIOSEQ_LIBSTDCXX_MODULES_JSON)
    set(_requested_modules "${BIOSEQ_LIBSTDCXX_MODULES_JSON}")
  endif()

  bioseq_select_gcc16_toolchain(
    REQUESTED_ROOT "${_requested_root}"
    REQUESTED_MODULES "${_requested_modules}"
    OUTPUT_ROOT _gcc_root
    OUTPUT_C _gcc_c
    OUTPUT_CXX _gcc_cxx
    OUTPUT_MODULES _gcc_modules
    OUTPUT_VERSION _gcc_version
    OUTPUT_C_SHA256 _gcc_c_sha256
    OUTPUT_CXX_SHA256 _gcc_cxx_sha256
    OUTPUT_MODULES_SHA256 _gcc_modules_sha256
    OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 _gcc_module_sources_sha256)
  _bioseq_highway_gcc_canary_require_homebrew_root("${_gcc_root}")
  _bioseq_highway_gcc_canary_driver_closure(
    _gcc_driver_closure_sha256 _gcc_libstdcxx _gcc_libgcc
    "${_gcc_root}" "${_gcc_cxx}")
  _bioseq_highway_gcc_canary_select_apple_sdk(
    CXX "${_gcc_cxx}"
    OUTPUT_DEVELOPER_DIR _apple_developer_dir
    OUTPUT_SDK_PATH _apple_sdk_path
    OUTPUT_SDK_ROOT _apple_sdk_root
    OUTPUT_VERSION _apple_sdk_version
    OUTPUT_BUILD_VERSION _apple_sdk_build_version
    OUTPUT_DIGEST _apple_sdk_digest)

  set(_nested_source
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../codegen/namespace_materialization/tests/integration/module_canary/gcc_canary")
  cmake_path(NORMAL_PATH _nested_source)
  if(NOT EXISTS "${_nested_source}/CMakeLists.txt")
    message(FATAL_ERROR
      "Highway GCC canary nested project is missing: ${_nested_source}")
  endif()
  set(_nested_build
    "${CMAKE_CURRENT_BINARY_DIR}/highway-gcc-canary/nested")
  set(_runner
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_bioseq_highway_gcc_canary.cmake")
  set(_toolchain_helper
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/bioseq_gcc_toolchain.cmake")

  add_custom_target("${ARG_TARGET}"
    COMMAND "/usr/bin/env" -i
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
      "LC_ALL=C"
      "LANG=C"
      "DEVELOPER_DIR=${_apple_developer_dir}"
      "SDKROOT=${_apple_sdk_root}"
      "${CMAKE_COMMAND}"
      "-DBIOSEQ_GCC_CANARY_CMAKE=${CMAKE_COMMAND}"
      "-DBIOSEQ_GCC_CANARY_NINJA=${CMAKE_MAKE_PROGRAM}"
      "-DBIOSEQ_GCC_CANARY_SOURCE=${_nested_source}"
      "-DBIOSEQ_GCC_CANARY_BUILD=${_nested_build}"
      "-DBIOSEQ_GCC_CANARY_OUTER_BINARY=${CMAKE_BINARY_DIR}"
      "-DBIOSEQ_GCC_CANARY_CANARY_HELPER=${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
      "-DBIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT=${_canary_source_root}"
      "-DBIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT=${_highway_include_root}"
      "-DBIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST=${_highway_header_digest}"
      "-DBIOSEQ_GCC_CANARY_GENERATED_ROOT=${ARG_GENERATED_ROOT}"
      "-DBIOSEQ_GCC_CANARY_PRIMARY=${ARG_PRIMARY}"
      "-DBIOSEQ_GCC_CANARY_DRIVER=${ARG_DRIVER}"
      "-DBIOSEQ_GCC_CANARY_MAIN=${ARG_MAIN}"
      "-DBIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER=${_toolchain_helper}"
      "-DBIOSEQ_GCC_CANARY_ROOT=${_gcc_root}"
      "-DBIOSEQ_GCC_CANARY_C=${_gcc_c}"
      "-DBIOSEQ_GCC_CANARY_CXX=${_gcc_cxx}"
      "-DBIOSEQ_GCC_CANARY_MODULES=${_gcc_modules}"
      "-DBIOSEQ_GCC_CANARY_VERSION=${_gcc_version}"
      "-DBIOSEQ_GCC_CANARY_C_SHA256=${_gcc_c_sha256}"
      "-DBIOSEQ_GCC_CANARY_CXX_SHA256=${_gcc_cxx_sha256}"
      "-DBIOSEQ_GCC_CANARY_MODULES_SHA256=${_gcc_modules_sha256}"
      "-DBIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256=${_gcc_module_sources_sha256}"
      "-DBIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256=${_gcc_driver_closure_sha256}"
      "-DBIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR=${_apple_developer_dir}"
      "-DBIOSEQ_GCC_CANARY_APPLE_SDK_PATH=${_apple_sdk_path}"
      "-DBIOSEQ_GCC_CANARY_APPLE_SDK_ROOT=${_apple_sdk_root}"
      "-DBIOSEQ_GCC_CANARY_APPLE_SDK_VERSION=${_apple_sdk_version}"
      "-DBIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION=${_apple_sdk_build_version}"
      "-DBIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST=${_apple_sdk_digest}"
      -P "${_runner}"
    COMMENT
      "Compiling and linking GCC Highway/module canaries with an empty environment"
    VERBATIM
    USES_TERMINAL)
  add_dependencies("${ARG_TARGET}" "${ARG_GENERATION_TARGET}")
endfunction()
