cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED TEST_ROOT OR "${TEST_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT DEFINED SOURCE_ROOT OR "${SOURCE_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

set(_canary_module "${SOURCE_ROOT}/cmake/bioseq_highway_gcc_canary.cmake")
set(_imported_binder_helper
  "${SOURCE_ROOT}/cmake/bioseq_highway_imported_binder_canary.cmake")
set(_toolchain_helper "${SOURCE_ROOT}/cmake/bioseq_gcc_toolchain.cmake")
set(_runner "${SOURCE_ROOT}/cmake/run_bioseq_highway_gcc_canary.cmake")
set(_outer_canary_project
  "${SOURCE_ROOT}/codegen/namespace_materialization/tests/integration/module_canary/CMakeLists.txt")
set(_nested_project
  "${SOURCE_ROOT}/codegen/namespace_materialization/tests/integration/module_canary/gcc_canary/CMakeLists.txt")
foreach(_checked IN ITEMS
    _canary_module _imported_binder_helper _toolchain_helper _runner
    _outer_canary_project _nested_project)
  if(NOT EXISTS "${${_checked}}")
    message(FATAL_ERROR "missing Highway GCC canary contract file: ${${_checked}}")
  endif()
endforeach()
file(READ "${_canary_module}" _canary_module_source)
file(READ "${_runner}" _runner_source)
foreach(_runner_identity_contract IN ITEMS
    "bioseq-highway-gcc-canary"
    "revision=2")
  string(FIND "${_runner_source}" "${_runner_identity_contract}"
    _runner_identity_offset)
  if(_runner_identity_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runner lost current identity framing: "
      "${_runner_identity_contract}")
  endif()
endforeach()
string(FIND "${_canary_module_source}"
  "_bioseq_highway_gcc_canary_header_digest"
  _header_authentication_offset)
if(_header_authentication_offset EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC canary must authenticate its complete Highway header closure")
endif()
foreach(_driver_closure_contract IN ITEMS
    "_bioseq_highway_gcc_canary_driver_closure"
    "-print-prog-name=cc1plus"
    "-print-prog-name=collect2"
    "-print-file-name=include"
    "-print-file-name=include-fixed"
    "-dumpspecs"
    "libstdc++.dylib"
    "libstdc++.a"
    "libgcc_s.1.1.dylib"
    "libgcc.a"
    "libgcc_eh.a"
    "include/c++"
    "gcc-internal"
    "gcc-include-fixed"
    "bioseq-highway-gcc-canary-headers"
    "bioseq-gcc-header-tree"
    "bioseq-highway-gcc-driver-closure"
    "revision=1"
    "revision=2"
    "_bioseq_highway_gcc_canary_collect_macho_closure")
  string(FIND "${_canary_module_source}" "${_driver_closure_contract}"
    _driver_closure_offset)
  if(_driver_closure_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary lost driver closure evidence "
      "${_driver_closure_contract}")
  endif()
endforeach()
foreach(_apple_sdk_contract IN ITEMS
    "_bioseq_highway_gcc_canary_select_apple_sdk"
    "_bioseq_highway_gcc_canary_verify_apple_sdk"
    "bioseq-highway-gcc-apple-sdk-c-headers"
    "bioseq-highway-gcc-apple-sdk"
    "/usr/bin/xcode-select"
    "/usr/bin/xcrun"
    "--show-sdk-path"
    "--show-sdk-version"
    "--show-sdk-build-version"
    "SDKSettings.plist"
    "SystemVersion.plist"
    "_bounds.h"
    "usr/lib/libSystem.tbd"
    "-include cstdio")
  string(FIND "${_canary_module_source}" "${_apple_sdk_contract}"
    _apple_sdk_offset)
  if(_apple_sdk_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary lost sealed Apple SDK contract "
      "${_apple_sdk_contract}")
  endif()
endforeach()
foreach(_macho_closure_contract IN ITEMS
    "/usr/bin/otool"
    "LC_LOAD_DYLIB"
    "LC_LOAD_WEAK_DYLIB"
    "LC_REEXPORT_DYLIB"
    "LC_LOAD_UPWARD_DYLIB"
    "LC_LAZY_LOAD_DYLIB"
    "LC_LOAD_DYLINKER"
    "LC_DYLD_ENVIRONMENT"
    "exact-account"
    "unparsed"
    [=[libc\\+\\+]=]
    "libhwy"
    "file(SHA256")
  string(FIND "${_canary_module_source}" "${_macho_closure_contract}"
    _macho_closure_offset)
  if(_macho_closure_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary lost recursive Mach-O closure evidence "
      "${_macho_closure_contract}")
  endif()
endforeach()
foreach(_outer_generic_contract IN ITEMS
    "GENERATION_TARGET"
    "GENERATED_ROOT"
    "add_dependencies")
  string(FIND "${_canary_module_source}" "${_outer_generic_contract}"
    _outer_generic_offset)
  if(_outer_generic_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary lost generic closure dependency "
      "${_outer_generic_contract}")
  endif()
endforeach()

function(_expect_script_failure name source expected)
  set(_script "${TEST_ROOT}/${name}.cmake")
  file(WRITE "${_script}" "${source}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${_script}"
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(_status EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly succeeded")
  endif()
  set(_combined "${_stdout}\n${_stderr}")
  if(NOT _combined MATCHES "${expected}")
    message(FATAL_ERROR
      "${name} did not fail with '${expected}':\n${_combined}")
  endif()
endfunction()

function(_require_ordered_text name source)
  set(_remaining "${source}")
  foreach(_needle IN LISTS ARGN)
    string(FIND "${_remaining}" "${_needle}" _needle_offset)
    if(_needle_offset EQUAL -1)
      message(FATAL_ERROR
        "${name} lost ordered contract text '${_needle}'")
    endif()
    string(LENGTH "${_needle}" _needle_length)
    math(EXPR _remaining_offset "${_needle_offset} + ${_needle_length}")
    string(SUBSTRING "${_remaining}" ${_remaining_offset} -1 _remaining)
  endforeach()
endfunction()

set(CANARY_MODULE "${_canary_module}")
set(CMAKE_EXE "${CMAKE_COMMAND}")
set(_off_darwin [=[
include("@CANARY_MODULE@")
_bioseq_highway_gcc_canary_validate_host(
  HOST_SYSTEM Linux
  TARGET_SYSTEM Linux
  COMPILER_ID Clang
  GENERATOR Ninja
  MAKE_PROGRAM "@CMAKE_EXE@")
]=])
string(CONFIGURE "${_off_darwin}" _off_darwin @ONLY)
_expect_script_failure(off_darwin "${_off_darwin}" "native-Darwin")

set(_non_clang [=[
include("@CANARY_MODULE@")
_bioseq_highway_gcc_canary_validate_host(
  HOST_SYSTEM Darwin
  TARGET_SYSTEM Darwin
  COMPILER_ID GNU
  GENERATOR Ninja
  MAKE_PROGRAM "@CMAKE_EXE@")
]=])
string(CONFIGURE "${_non_clang}" _non_clang @ONLY)
_expect_script_failure(non_clang_host "${_non_clang}" "ordinary Clang")

set(_fake_root "${TEST_ROOT}/fake-gcc-16.1")
file(MAKE_DIRECTORY "${_fake_root}/bin" "${_fake_root}/lib")
file(WRITE "${_fake_root}/lib/std.cc" "export module std;\n")
file(WRITE "${_fake_root}/lib/std.compat.cc" "export module std.compat;\n")
set(_inventory "${_fake_root}/lib/libstdc++.modules.json")
file(WRITE "${_inventory}"
  "{\n"
  "  \"version\": 1,\n"
  "  \"modules\": [\n"
  "    {\"logical-name\":\"std\",\"source-path\":\"std.cc\",\"is-std-library\":true},\n"
  "    {\"logical-name\":\"std.compat\",\"source-path\":\"std.compat.cc\",\"is-std-library\":true}\n"
  "  ]\n"
  "}\n")
set(_fake_compiler
  "#!/bin/sh\n"
  "case \"$1\" in\n"
  "  -dumpfullversion|-dumpversion) printf '%s\\n' '16.1.0' ;;\n"
  "  -print-file-name=libstdc++.modules.json) printf '%s\\n' '${_inventory}' ;;\n"
  "  *) exit 0 ;;\n"
  "esac\n")
foreach(_compiler IN ITEMS gcc-16 g++-16)
  file(WRITE "${_fake_root}/bin/${_compiler}" "${_fake_compiler}")
  file(CHMOD "${_fake_root}/bin/${_compiler}"
    PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
endforeach()

include("${_toolchain_helper}")
bioseq_select_gcc16_toolchain(
  REQUESTED_ROOT "${_fake_root}"
  REQUESTED_MODULES "${_inventory}"
  OUTPUT_ROOT _selected_root
  OUTPUT_C _selected_c
  OUTPUT_CXX _selected_cxx
  OUTPUT_MODULES _selected_modules
  OUTPUT_VERSION _selected_version
  OUTPUT_C_SHA256 _selected_c_sha256
  OUTPUT_CXX_SHA256 _selected_cxx_sha256
  OUTPUT_MODULES_SHA256 _selected_modules_sha256
  OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 _selected_sources_sha256)
if(NOT _selected_version VERSION_EQUAL "16.1.0"
    OR NOT _selected_modules STREQUAL "${_inventory}"
    OR "${_selected_c_sha256}" STREQUAL ""
    OR "${_selected_cxx_sha256}" STREQUAL ""
    OR "${_selected_modules_sha256}" STREQUAL ""
    OR "${_selected_sources_sha256}" STREQUAL "")
  message(FATAL_ERROR
    "authenticated fake GCC compiler/inventory selection was incomplete")
endif()
bioseq_verify_gcc16_toolchain_identity(
  ROOT "${_selected_root}"
  C "${_selected_c}"
  CXX "${_selected_cxx}"
  MODULES "${_selected_modules}"
  VERSION "${_selected_version}"
  C_SHA256 "${_selected_c_sha256}"
  CXX_SHA256 "${_selected_cxx_sha256}"
  MODULES_SHA256 "${_selected_modules_sha256}"
  MODULE_SOURCE_CLOSURE_SHA256 "${_selected_sources_sha256}")

set(TOOLCHAIN_HELPER "${_toolchain_helper}")
set(MISSING_ROOT "${TEST_ROOT}/missing-gcc")
file(MAKE_DIRECTORY "${MISSING_ROOT}")
set(_missing_pair [=[
include("@TOOLCHAIN_HELPER@")
bioseq_select_gcc16_toolchain(
  REQUESTED_ROOT "@MISSING_ROOT@"
  OUTPUT_ROOT root
  OUTPUT_C c
  OUTPUT_CXX cxx
  OUTPUT_MODULES modules
  OUTPUT_VERSION version
  OUTPUT_C_SHA256 c_sha
  OUTPUT_CXX_SHA256 cxx_sha
  OUTPUT_MODULES_SHA256 modules_sha
  OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 sources_sha)
]=])
string(CONFIGURE "${_missing_pair}" _missing_pair @ONLY)
_expect_script_failure(
  missing_gcc_pair "${_missing_pair}" "paired gcc-16/g\\+\\+-16")

function(_create_fake_apple_sdk developer output_path output_root)
  set(_sdk_parent
    "${developer}/Platforms/MacOSX.platform/Developer/SDKs")
  set(_sdk_root "${_sdk_parent}/MacOSX26.2.sdk")
  set(_sdk_path "${_sdk_parent}/MacOSX.sdk")
  file(MAKE_DIRECTORY
    "${_sdk_root}/usr/include/detail"
    "${_sdk_root}/usr/include/sys"
    "${_sdk_root}/usr/lib"
    "${_sdk_root}/System/Library/CoreServices"
    "${_sdk_root}/System/Library/Frameworks/Fake.framework/Headers")

  set(_sdk_settings [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Version</key>
  <string>26.2</string>
  <key>CanonicalName</key>
  <string>macosx26.2</string>
</dict>
</plist>
]=])
  set(_system_version [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>ProductVersion</key>
  <string>26.2</string>
  <key>ProductBuildVersion</key>
  <string>TEST26A1</string>
</dict>
</plist>
]=])
  file(WRITE "${_sdk_root}/SDKSettings.plist" "${_sdk_settings}")
  file(WRITE "${_sdk_root}/SDKSettings.json"
    "{\n  \"Version\": \"26.2\",\n  \"CanonicalName\": \"macosx26.2\"\n}\n")
  file(WRITE
    "${_sdk_root}/System/Library/CoreServices/SystemVersion.plist"
    "${_system_version}")
  foreach(_header IN ITEMS
      _bounds.h stdio.h stdlib.h stddef.h stdint.h sys/types.h)
    file(WRITE "${_sdk_root}/usr/include/${_header}"
      "/* sealed fake Apple SDK ${_header} */\n")
  endforeach()
  file(WRITE "${_sdk_root}/usr/include/detail/non_sentinel.h"
    "#define BIOSEQ_FAKE_APPLE_SDK_SENTINEL 1\n")
  file(WRITE
    "${_sdk_root}/System/Library/Frameworks/Fake.framework/Headers/aliased.h"
    "#define BIOSEQ_FAKE_APPLE_SDK_ALIASED_HEADER 1\n")
  file(CREATE_LINK
    "${_sdk_root}/System/Library/Frameworks/Fake.framework/Headers"
    "${_sdk_root}/usr/include/fake_framework" SYMBOLIC
    RESULT _framework_header_link_status)
  if(NOT _framework_header_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not create fake Apple SDK framework-header link: "
      "${_framework_header_link_status}")
  endif()
  file(WRITE "${_sdk_root}/usr/lib/libSystem.B.tbd"
    "--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-macos ]\n...\n")
  file(CREATE_LINK
    "${_sdk_root}/usr/lib/libSystem.B.tbd"
    "${_sdk_root}/usr/lib/libSystem.tbd" SYMBOLIC
    RESULT _libsystem_link_status)
  if(NOT _libsystem_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not create fake Apple SDK libSystem link: "
      "${_libsystem_link_status}")
  endif()
  file(CREATE_LINK "${_sdk_root}" "${_sdk_path}" SYMBOLIC
    RESULT _sdk_link_status)
  if(NOT _sdk_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not create fake active Apple SDK link: ${_sdk_link_status}")
  endif()
  set(${output_path} "${_sdk_path}" PARENT_SCOPE)
  set(${output_root} "${_sdk_root}" PARENT_SCOPE)
endfunction()

function(_write_fake_apple_sdk_compiler
    compiler developer sdk_root receipt)
  set(EXPECTED_DEVELOPER "${developer}")
  set(EXPECTED_SDK_ROOT "${sdk_root}")
  set(PROBE_RECEIPT "${receipt}")
  set(_probe_template [=[#!/bin/sh
fail() {
  printf '%s\n' "$1" >&2
  exit 91
}
[ "$#" -eq 9 ] || fail "unexpected argc=$#"
[ "$1" = "-std=gnu++26" ] || fail "unexpected argv[1]=$1"
[ "$2" = "-isysroot" ] || fail "unexpected argv[2]=$2"
[ "$3" = "@EXPECTED_SDK_ROOT@" ] || fail "unexpected argv[3]=$3"
[ "$4" = "-x" ] || fail "unexpected argv[4]=$4"
[ "$5" = "c++" ] || fail "unexpected argv[5]=$5"
[ "$6" = "-include" ] || fail "unexpected argv[6]=$6"
[ "$7" = "cstdio" ] || fail "unexpected argv[7]=$7"
[ "$8" = "-fsyntax-only" ] || fail "unexpected argv[8]=$8"
[ "$9" = "/dev/null" ] || fail "unexpected argv[9]=$9"
[ "${PATH-}" = "/usr/bin:/bin:/usr/sbin:/sbin" ] || fail "unexpected PATH"
[ "${LC_ALL-}" = "C" ] || fail "unexpected LC_ALL"
[ "${LANG-}" = "C" ] || fail "unexpected LANG"
[ "${DEVELOPER_DIR-}" = "@EXPECTED_DEVELOPER@" ] || fail "unexpected DEVELOPER_DIR"
[ "${SDKROOT-}" = "@EXPECTED_SDK_ROOT@" ] || fail "unexpected SDKROOT"
if [ -n "${CPATH-}" ] || [ -n "${C_INCLUDE_PATH-}" ] || \
   [ -n "${CPLUS_INCLUDE_PATH-}" ] || [ -n "${OBJC_INCLUDE_PATH-}" ] || \
   [ -n "${GCC_EXEC_PREFIX-}" ] || [ -n "${COMPILER_PATH-}" ] || \
   [ -n "${LIBRARY_PATH-}" ] || [ -n "${DYLD_INSERT_LIBRARIES-}" ] || \
   [ -n "${DYLD_LIBRARY_PATH-}" ] || [ -n "${DYLD_FRAMEWORK_PATH-}" ]; then
  fail "forbidden ambient compiler environment survived"
fi
printf '%s\n' probe-ok > "@PROBE_RECEIPT@"
]=])
  string(CONFIGURE "${_probe_template}" _probe_source @ONLY)
  file(WRITE "${compiler}" "${_probe_source}")
  file(CHMOD "${compiler}"
    PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
endfunction()

function(_expect_fake_sdk_closure_failure name sdk_path sdk_root expected)
  set(CASE_CANARY_MODULE "${_canary_module}")
  set(CASE_COMPILER "${_fake_sdk_compiler}")
  set(CASE_DEVELOPER "${_fake_developer}")
  set(CASE_SDK_PATH "${sdk_path}")
  set(CASE_SDK_ROOT "${sdk_root}")
  set(_case_source [=[
include("@CASE_CANARY_MODULE@")
_bioseq_highway_gcc_canary_apple_sdk_closure(
  CXX "@CASE_COMPILER@"
  DEVELOPER_DIR "@CASE_DEVELOPER@"
  SDK_PATH "@CASE_SDK_PATH@"
  SDK_ROOT "@CASE_SDK_ROOT@"
  OUTPUT_VERSION observed_version
  OUTPUT_BUILD_VERSION observed_build
  OUTPUT_DIGEST observed_digest)
]=])
  string(CONFIGURE "${_case_source}" _configured_case @ONLY)
  _expect_script_failure("${name}" "${_configured_case}" "${expected}")
endfunction()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  include("${_canary_module}")
  set(_fake_developer "${TEST_ROOT}/Fake Developer With Spaces")
  _create_fake_apple_sdk(
    "${_fake_developer}" _fake_sdk_path _fake_sdk_root)
  file(REAL_PATH "${_fake_developer}" _fake_developer_real)
  file(REAL_PATH "${_fake_sdk_root}" _fake_sdk_root_real)
  set(_fake_developer "${_fake_developer_real}")
  set(_fake_sdk_root "${_fake_sdk_root_real}")
  set(_fake_sdk_compiler
    "${TEST_ROOT}/Fake Compiler With Spaces/g++-16")
  get_filename_component(_fake_sdk_compiler_dir
    "${_fake_sdk_compiler}" DIRECTORY)
  file(MAKE_DIRECTORY "${_fake_sdk_compiler_dir}")
  set(_fake_probe_receipt "${TEST_ROOT}/fake-sdk-probe.receipt")
  _write_fake_apple_sdk_compiler(
    "${_fake_sdk_compiler}" "${_fake_developer}" "${_fake_sdk_root}"
    "${_fake_probe_receipt}")

  set(_poisoned_sdk_environment
    DEVELOPER_DIR SDKROOT CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
    OBJC_INCLUDE_PATH GCC_EXEC_PREFIX COMPILER_PATH LIBRARY_PATH
    DYLD_INSERT_LIBRARIES DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH)
  foreach(_poisoned_name IN LISTS _poisoned_sdk_environment)
    set(ENV{${_poisoned_name}} "/poisoned/${_poisoned_name}")
  endforeach()

  file(REMOVE "${_fake_probe_receipt}")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _fake_sdk_version
    OUTPUT_BUILD_VERSION _fake_sdk_build
    OUTPUT_DIGEST _fake_sdk_digest)
  string(LENGTH "${_fake_sdk_digest}" _fake_sdk_digest_length)
  if(NOT _fake_sdk_version STREQUAL "26.2"
      OR NOT _fake_sdk_build STREQUAL "TEST26A1"
      OR NOT _fake_sdk_digest_length EQUAL 64
      OR NOT _fake_sdk_digest MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "fake Apple SDK closure returned an incomplete identity: "
      "${_fake_sdk_version}/${_fake_sdk_build}/${_fake_sdk_digest}")
  endif()
  if(NOT EXISTS "${_fake_probe_receipt}")
    message(FATAL_ERROR
      "fake Apple SDK closure did not run the exact compiler probe")
  endif()
  file(READ "${_fake_probe_receipt}" _fake_probe_result)
  if(NOT _fake_probe_result STREQUAL "probe-ok\n")
    message(FATAL_ERROR
      "fake Apple SDK compiler probe returned an unexpected receipt")
  endif()

  file(REMOVE "${_fake_probe_receipt}")
  _expect_fake_sdk_closure_failure(
    missing_apple_sdk
    "${TEST_ROOT}/missing Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    "${TEST_ROOT}/missing Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.2.sdk"
    "active Apple SDK path")
  if(EXISTS "${_fake_probe_receipt}")
    message(FATAL_ERROR "missing Apple SDK unexpectedly reached the compiler")
  endif()

  set(_bounds_header "${_fake_sdk_root}/usr/include/_bounds.h")
  file(READ "${_bounds_header}" _bounds_contents)
  file(REMOVE "${_bounds_header}" "${_fake_probe_receipt}")
  _expect_fake_sdk_closure_failure(
    missing_apple_sdk_bounds_header
    "${_fake_sdk_path}" "${_fake_sdk_root}" "_bounds\\.h")
  if(EXISTS "${_fake_probe_receipt}")
    message(FATAL_ERROR
      "missing _bounds.h unexpectedly reached the compiler probe")
  endif()
  file(WRITE "${_bounds_header}" "${_bounds_contents}")

  set(_sdk_settings "${_fake_sdk_root}/SDKSettings.plist")
  file(READ "${_sdk_settings}" _sdk_settings_contents)
  set(_escaped_sdk_settings "${TEST_ROOT}/escaped-SDKSettings.plist")
  file(WRITE "${_escaped_sdk_settings}" "${_sdk_settings_contents}")
  file(REMOVE "${_sdk_settings}" "${_fake_probe_receipt}")
  file(CREATE_LINK "${_escaped_sdk_settings}" "${_sdk_settings}" SYMBOLIC
    RESULT _escaped_sdk_settings_link_status)
  if(NOT _escaped_sdk_settings_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not create escaping fake SDK metadata link: "
      "${_escaped_sdk_settings_link_status}")
  endif()
  _expect_fake_sdk_closure_failure(
    escaped_apple_sdk_metadata
    "${_fake_sdk_path}" "${_fake_sdk_root}"
    "metadata must be")
  if(EXISTS "${_fake_probe_receipt}")
    message(FATAL_ERROR
      "escaping SDK metadata unexpectedly reached the compiler probe")
  endif()
  file(REMOVE "${_sdk_settings}")
  file(WRITE "${_sdk_settings}" "${_sdk_settings_contents}")

  set(_extra_header
    "${_fake_sdk_root}/usr/include/detail/non_sentinel.h")
  file(READ "${_extra_header}" _extra_header_contents)
  set(_escaped_header "${TEST_ROOT}/escaped-apple-sdk-header.h")
  file(WRITE "${_escaped_header}" "/* outside the fake SDK */\n")
  file(REMOVE "${_extra_header}" "${_fake_probe_receipt}")
  file(CREATE_LINK "${_escaped_header}" "${_extra_header}" SYMBOLIC
    RESULT _escaped_header_link_status)
  if(NOT _escaped_header_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not create escaping fake SDK header link: "
      "${_escaped_header_link_status}")
  endif()
  _expect_fake_sdk_closure_failure(
    escaped_apple_sdk_header
    "${_fake_sdk_path}" "${_fake_sdk_root}"
    "header escaped")
  if(EXISTS "${_fake_probe_receipt}")
    message(FATAL_ERROR
      "escaping SDK header unexpectedly reached the compiler probe")
  endif()
  file(REMOVE "${_extra_header}")
  file(WRITE "${_extra_header}" "${_extra_header_contents}")

  set(_wrong_sdk_root
    "${_fake_developer}/Platforms/MacOSX.platform/Developer/SDKs/Wrong.sdk")
  file(MAKE_DIRECTORY "${_wrong_sdk_root}")
  _expect_fake_sdk_closure_failure(
    mismatched_apple_sdk_root
    "${_fake_sdk_path}" "${_wrong_sdk_root}"
    "spelling no longer")

  file(APPEND "${_extra_header}" "/* mutated */\n")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _mutated_header_version
    OUTPUT_BUILD_VERSION _mutated_header_build
    OUTPUT_DIGEST _mutated_header_digest)
  if(_mutated_header_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "non-sentinel Apple SDK header mutation did not change the digest")
  endif()
  file(WRITE "${_extra_header}" "${_extra_header_contents}")

  set(_aliased_header
    "${_fake_sdk_root}/System/Library/Frameworks/Fake.framework/Headers/aliased.h")
  file(READ "${_aliased_header}" _aliased_header_contents)
  file(APPEND "${_aliased_header}" "/* mutated through directory alias */\n")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _mutated_alias_version
    OUTPUT_BUILD_VERSION _mutated_alias_build
    OUTPUT_DIGEST _mutated_alias_digest)
  if(_mutated_alias_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "symlinked Apple SDK header-directory mutation escaped the closure")
  endif()
  file(WRITE "${_aliased_header}" "${_aliased_header_contents}")

  set(_second_aliased_header
    "${_fake_sdk_root}/System/Library/Frameworks/Fake2.framework/Headers/aliased.h")
  get_filename_component(_second_aliased_header_dir
    "${_second_aliased_header}" DIRECTORY)
  file(MAKE_DIRECTORY "${_second_aliased_header_dir}")
  file(WRITE "${_second_aliased_header}" "${_aliased_header_contents}")
  get_filename_component(_original_aliased_header_dir
    "${_aliased_header}" DIRECTORY)
  file(REAL_PATH "${_original_aliased_header_dir}" _original_alias_target)
  file(REAL_PATH "${_second_aliased_header_dir}" _second_alias_target)
  file(GLOB _original_alias_files RELATIVE "${_original_alias_target}"
    "${_original_alias_target}/*")
  file(GLOB _second_alias_files RELATIVE "${_second_alias_target}"
    "${_second_alias_target}/*")
  list(SORT _original_alias_files)
  list(SORT _second_alias_files)
  if(NOT _original_alias_files STREQUAL _second_alias_files)
    message(FATAL_ERROR
      "fake Apple SDK directory-link retarget fixtures differ in topology")
  endif()
  foreach(_alias_file IN LISTS _original_alias_files)
    file(SHA256 "${_original_alias_target}/${_alias_file}" _original_alias_sha)
    file(SHA256 "${_second_alias_target}/${_alias_file}" _second_alias_sha)
    if(NOT _original_alias_sha STREQUAL _second_alias_sha)
      message(FATAL_ERROR
        "fake Apple SDK directory-link retarget fixtures differ in bytes")
    endif()
  endforeach()
  set(_header_directory_link
    "${_fake_sdk_root}/usr/include/fake_framework")
  file(REMOVE "${_header_directory_link}")
  file(CREATE_LINK "${_second_aliased_header_dir}"
    "${_header_directory_link}" SYMBOLIC
    RESULT _retargeted_header_directory_status)
  if(NOT _retargeted_header_directory_status STREQUAL "0")
    message(FATAL_ERROR
      "could not retarget fake Apple SDK header directory link: "
      "${_retargeted_header_directory_status}")
  endif()
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _retargeted_alias_version
    OUTPUT_BUILD_VERSION _retargeted_alias_build
    OUTPUT_DIGEST _retargeted_alias_digest)
  if(_retargeted_alias_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "Apple SDK header-directory symlink retargeting was not sealed")
  endif()
  file(REMOVE "${_header_directory_link}")
  file(CREATE_LINK "${_original_aliased_header_dir}"
    "${_header_directory_link}" SYMBOLIC
    RESULT _restored_header_directory_status)
  if(NOT _restored_header_directory_status STREQUAL "0")
    message(FATAL_ERROR
      "could not restore fake Apple SDK header directory link: "
      "${_restored_header_directory_status}")
  endif()

  string(REPLACE "</dict>"
    "  <key>Mutation</key>\n  <string>detected</string>\n</dict>"
    _mutated_sdk_settings "${_sdk_settings_contents}")
  file(WRITE "${_sdk_settings}" "${_mutated_sdk_settings}")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _mutated_settings_version
    OUTPUT_BUILD_VERSION _mutated_settings_build
    OUTPUT_DIGEST _mutated_settings_digest)
  if(_mutated_settings_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "Apple SDK settings mutation did not change the digest")
  endif()
  file(WRITE "${_sdk_settings}" "${_sdk_settings_contents}")

  set(_system_version_plist
    "${_fake_sdk_root}/System/Library/CoreServices/SystemVersion.plist")
  file(READ "${_system_version_plist}" _system_version_contents)
  string(REPLACE "TEST26A1" "TEST26A2"
    _mutated_system_version "${_system_version_contents}")
  file(WRITE "${_system_version_plist}" "${_mutated_system_version}")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _mutated_system_version_value
    OUTPUT_BUILD_VERSION _mutated_system_build
    OUTPUT_DIGEST _mutated_system_digest)
  if(NOT _mutated_system_build STREQUAL "TEST26A2"
      OR _mutated_system_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "Apple SDK build identity mutation was not sealed")
  endif()
  file(WRITE "${_system_version_plist}" "${_system_version_contents}")

  set(_libsystem_target "${_fake_sdk_root}/usr/lib/libSystem.B.tbd")
  file(READ "${_libsystem_target}" _libsystem_contents)
  file(APPEND "${_libsystem_target}" "# mutated\n")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_fake_sdk_compiler}"
    DEVELOPER_DIR "${_fake_developer}"
    SDK_PATH "${_fake_sdk_path}"
    SDK_ROOT "${_fake_sdk_root}"
    OUTPUT_VERSION _mutated_libsystem_version
    OUTPUT_BUILD_VERSION _mutated_libsystem_build
    OUTPUT_DIGEST _mutated_libsystem_digest)
  if(_mutated_libsystem_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "Apple SDK libSystem mutation did not change the digest")
  endif()
  file(WRITE "${_libsystem_target}" "${_libsystem_contents}")

  set(_libsystem_link "${_fake_sdk_root}/usr/lib/libSystem.tbd")
  set(_escaped_libsystem "${TEST_ROOT}/escaped-libSystem.tbd")
  file(WRITE "${_escaped_libsystem}" "--- !tapi-tbd\n...\n")
  file(REMOVE "${_libsystem_link}" "${_fake_probe_receipt}")
  file(CREATE_LINK "${_escaped_libsystem}" "${_libsystem_link}" SYMBOLIC
    RESULT _escaped_libsystem_link_status)
  if(NOT _escaped_libsystem_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not create escaping fake libSystem link: "
      "${_escaped_libsystem_link_status}")
  endif()
  _expect_fake_sdk_closure_failure(
    escaped_apple_sdk_libsystem
    "${_fake_sdk_path}" "${_fake_sdk_root}"
    "libSystem\\.tbd escaped the active Apple SDK")
  if(EXISTS "${_fake_probe_receipt}")
    message(FATAL_ERROR
      "escaping libSystem unexpectedly reached the compiler probe")
  endif()
  file(REMOVE "${_libsystem_link}")
  file(CREATE_LINK "${_libsystem_target}" "${_libsystem_link}" SYMBOLIC
    RESULT _restored_libsystem_link_status)
  if(NOT _restored_libsystem_link_status STREQUAL "0")
    message(FATAL_ERROR
      "could not restore fake SDK libSystem link: "
      "${_restored_libsystem_link_status}")
  endif()

  set(_relocated_developer
    "${TEST_ROOT}/Relocated Fake Developer With Spaces")
  _create_fake_apple_sdk(
    "${_relocated_developer}" _relocated_sdk_path _relocated_sdk_root)
  file(REAL_PATH "${_relocated_developer}" _relocated_developer_real)
  file(REAL_PATH "${_relocated_sdk_root}" _relocated_sdk_root_real)
  set(_relocated_developer "${_relocated_developer_real}")
  set(_relocated_sdk_root "${_relocated_sdk_root_real}")
  set(_relocated_compiler
    "${TEST_ROOT}/Relocated Fake Compiler With Spaces/g++-16")
  get_filename_component(_relocated_compiler_dir
    "${_relocated_compiler}" DIRECTORY)
  file(MAKE_DIRECTORY "${_relocated_compiler_dir}")
  set(_relocated_receipt "${TEST_ROOT}/relocated-sdk-probe.receipt")
  _write_fake_apple_sdk_compiler(
    "${_relocated_compiler}" "${_relocated_developer}"
    "${_relocated_sdk_root}" "${_relocated_receipt}")
  _bioseq_highway_gcc_canary_apple_sdk_closure(
    CXX "${_relocated_compiler}"
    DEVELOPER_DIR "${_relocated_developer}"
    SDK_PATH "${_relocated_sdk_path}"
    SDK_ROOT "${_relocated_sdk_root}"
    OUTPUT_VERSION _relocated_sdk_version
    OUTPUT_BUILD_VERSION _relocated_sdk_build
    OUTPUT_DIGEST _relocated_sdk_digest)
  if(_relocated_sdk_digest STREQUAL _fake_sdk_digest)
    message(FATAL_ERROR
      "relocated byte-identical Apple SDK retained the old path identity")
  endif()

  foreach(_poisoned_name IN LISTS _poisoned_sdk_environment)
    unset(ENV{${_poisoned_name}})
  endforeach()
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" _root_cmake)
string(FIND "${_root_cmake}" "option(BIOSEQ_HIGHWAY_GCC_CANARY" _option_offset)
if(_option_offset EQUAL -1)
  message(FATAL_ERROR "Highway GCC canary option is missing")
endif()
string(FIND "${_root_cmake}"
  "module boundary canary\" OFF)" _option_off_offset)
if(_option_off_offset EQUAL -1)
  message(FATAL_ERROR "Highway GCC canary must remain OFF by default")
endif()
foreach(_selection_contract IN ITEMS
    [=[OR NOT IS_ABSOLUTE "${_developer_path}"]=]
    [=[COMMAND ${_clean_environment} "/usr/bin/xcode-select" -p]=]
    [=[${_clean_environment} "DEVELOPER_DIR=${_developer_real}"]=]
    [=["/usr/bin/xcrun" --sdk macosx "${_query_argument}"]=]
    [=[--show-sdk-path --show-sdk-version --show-sdk-build-version]=]
    [=[file(GLOB_RECURSE _sdk_headers FOLLOW_SYMLINKS]=]
    [=[file(GLOB_RECURSE _sdk_header_entries LIST_DIRECTORIES TRUE]=]
    [=[bioseq-highway-gcc-apple-sdk-c-header-symlinks]=]
    [=[c-header-symlinks=${_header_symlink_digest}]=]
    [=["/usr/bin/plutil" -extract "${_query_key}" raw -o -]=]
    [=["DEVELOPER_DIR=${_developer_real}"]=]
    [=["SDKROOT=${_sdk_real}"]=]
    [=[-std=gnu++26]=]
    [=[-isysroot "${_sdk_real}"]=]
    [=[-x c++]=]
    [=[-include cstdio]=]
    [=[-fsyntax-only /dev/null]=])
  string(FIND "${_canary_module_source}" "${_selection_contract}"
    _selection_contract_offset)
  if(_selection_contract_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK selection lost exact contract text: "
      "${_selection_contract}")
  endif()
endforeach()
foreach(_forbidden_selection_text IN ITEMS
    [=[$ENV{DEVELOPER_DIR}]=]
    [=[$ENV{SDKROOT}]=]
    [=[find_program(]=]
    [=[/Applications/Xcode]=]
    [=[/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk]=])
  string(FIND "${_canary_module_source}" "${_forbidden_selection_text}"
    _forbidden_selection_offset)
  if(NOT _forbidden_selection_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary Apple SDK selection retained ambient/discovery "
      "text: ${_forbidden_selection_text}")
  endif()
endforeach()
_require_ordered_text(
  "Highway GCC canary host-before-SDK selection"
  "${_canary_module_source}"
  [=[_bioseq_highway_gcc_canary_validate_host(]=]
  [=[bioseq_select_gcc16_toolchain(]=]
  [=[_bioseq_highway_gcc_canary_select_apple_sdk(]=]
  [=[add_custom_target(]=])

foreach(_outer_sdk_argument IN ITEMS
    [=["DEVELOPER_DIR=${_apple_developer_dir}"]=]
    [=["SDKROOT=${_apple_sdk_root}"]=]
    [=["-DBIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR=${_apple_developer_dir}"]=]
    [=["-DBIOSEQ_GCC_CANARY_APPLE_SDK_PATH=${_apple_sdk_path}"]=]
    [=["-DBIOSEQ_GCC_CANARY_APPLE_SDK_ROOT=${_apple_sdk_root}"]=]
    [=["-DBIOSEQ_GCC_CANARY_APPLE_SDK_VERSION=${_apple_sdk_version}"]=]
    [=["-DBIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION=${_apple_sdk_build_version}"]=]
    [=["-DBIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST=${_apple_sdk_digest}"]=])
  string(FIND "${_canary_module_source}" "${_outer_sdk_argument}"
    _outer_sdk_argument_offset)
  if(_outer_sdk_argument_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary outer target lost sealed Apple SDK argument: "
      "${_outer_sdk_argument}")
  endif()
endforeach()

file(READ "${_nested_project}" _nested)
foreach(_required_text IN ITEMS
    "CMAKE_CXX_MODULE_STD OFF"
    "CXX_MODULE_STD OFF"
    "BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT"
    "BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST"
    "bioseq_highway_imported_binder_canary.cmake"
    "bioseq_add_highway_imported_binder_canary"
    "TARGET bioseq_highway_gcc_module_canary"
    "typed_flat_api.cppm"
    "instance_3.cppm"
    "bioseq_namespace_materialization_gcc_module_canary"
    "bioseq_gcc_canary_closure")
  string(FIND "${_nested}" "${_required_text}" _required_offset)
  if(_required_offset EQUAL -1)
    message(FATAL_ERROR
      "nested GCC canary lost required closure text: ${_required_text}")
  endif()
endforeach()

file(READ "${_imported_binder_helper}" _imported_binder_helper_source)
foreach(_required_text IN ITEMS
    "bioseq_add_highway_imported_binder_canary"
    "HWY_NEON_WITHOUT_AES"
    "HWY_NEON"
    "HWY_NEON_BF16"
    "HWY_SSE2"
    "HWY_SSE4"
    "HWY_EMU128"
    "CMAKE_OSX_ARCHITECTURES"
    "multi-architecture"
    "no fail-closed native target pair"
    "PLATFORM_HEADER_OVERRIDES"
    "TARGET_DISTINGUISHING_PROBE_KIND 2"
    "CMAKE_SYSTEM_NAME STREQUAL \"Darwin\""
    "CMAKE_SYSTEM_NAME STREQUAL \"Linux\""
    "target_attribute_shapes.cppm.in"
    "attribute_shapes.cppm"
    "highway_registry.cppm"
    "runtime.cpp"
    "BASE_DIRS"
    "CXX_SCAN_FOR_MODULES ON")
  string(FIND "${_imported_binder_helper_source}" "${_required_text}"
    _required_offset)
  if(_required_offset EQUAL -1)
    message(FATAL_ERROR
      "shared imported-binder canary lost required closure text: "
      "${_required_text}")
  endif()
endforeach()
foreach(_target_policy IN ITEMS
    [=["${SIMD_TARGET_TOKEN} | HWY_EMU128"]=]
    [=["${SIMD_TARGET_TOKEN} | ${ATTRIBUTED_TARGET_TOKEN} | HWY_EMU128"]=])
  string(FIND "${_imported_binder_helper_source}" "${_target_policy}"
    _target_policy_offset)
  if(_target_policy_offset EQUAL -1)
    message(FATAL_ERROR
      "shared imported-binder canary lost exact target policy "
      "${_target_policy}")
  endif()
endforeach()
string(FIND "${_nested}" "find_package(" _find_package_offset)
string(FIND "${_nested}" "target_link_libraries(" _link_libraries_offset)
string(FIND "${_imported_binder_helper_source}" "find_package("
  _helper_find_package_offset)
string(FIND "${_imported_binder_helper_source}" "target_link_libraries("
  _helper_link_libraries_offset)
if(NOT _find_package_offset EQUAL -1
    OR NOT _link_libraries_offset EQUAL -1
    OR NOT _helper_find_package_offset EQUAL -1
    OR NOT _helper_link_libraries_offset EQUAL -1)
  message(FATAL_ERROR
    "shared/nested canary must have no package discovery or explicit libraries")
endif()

file(READ "${_outer_canary_project}" _outer_canary_source)
foreach(_native_canary_contract IN ITEMS
    [=[CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$"]=]
    "include(bioseq_highway_imported_binder_canary)"
    "TARGET bioseq_highway_clang_imported_binder_canary"
    "highway-clang-canary/generated"
    "highway_canary")
  string(FIND "${_outer_canary_source}" "${_native_canary_contract}"
    _native_canary_offset)
  if(_native_canary_offset EQUAL -1)
    message(FATAL_ERROR
      "ordinary Clang build lost imported-binder canary contract "
      "${_native_canary_contract}")
  endif()
endforeach()
string(FIND "${_root_cmake}"
  "bioseq_highway_clang_imported_binder_canary"
  _native_consumer_dependency_offset)
if(_native_consumer_dependency_offset EQUAL -1)
  message(FATAL_ERROR
    "normal build-only consumers lost the Clang imported-binder dependency")
endif()

foreach(_nested_sdk_contract IN ITEMS
    "BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_PATH"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST"
    [=[set(CMAKE_OSX_SYSROOT "${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"]=]
    [=[_CMAKE_OSX_SYSROOT_PATH]=]
    [=[CMAKE_CXX_SYSROOT_FLAG STREQUAL "-isysroot"]=]
    [=[CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK]=]
    [=[forbids compiler launchers and ambient flags]=])
  string(FIND "${_nested}" "${_nested_sdk_contract}"
    _nested_sdk_contract_offset)
  if(_nested_sdk_contract_offset EQUAL -1)
    message(FATAL_ERROR
      "nested GCC canary lost sealed Apple SDK contract: "
      "${_nested_sdk_contract}")
  endif()
endforeach()
_require_ordered_text(
  "nested GCC pre-project Apple SDK verification"
  "${_nested}"
  [=[set(CMAKE_CXX_COMPILER]=]
  [=[set(CMAKE_OSX_SYSROOT]=]
  [=[include("${BIOSEQ_GCC_CANARY_CANARY_HELPER}")]=]
  [=[_bioseq_highway_gcc_canary_verify_apple_sdk(]=]
  [=[project(bioseq_highway_gcc_module_canary LANGUAGES CXX)]=])
string(FIND "${_nested}"
  [=[project(bioseq_highway_gcc_module_canary LANGUAGES CXX)]=]
  _nested_project_offset)
string(FIND "${_nested}" [=[_CMAKE_OSX_SYSROOT_PATH]=]
  _nested_resolved_sdk_offset)
string(FIND "${_nested}" [=[CMAKE_CXX_SYSROOT_FLAG STREQUAL "-isysroot"]=]
  _nested_sysroot_flag_offset)
if(_nested_project_offset EQUAL -1
    OR _nested_resolved_sdk_offset LESS _nested_project_offset
    OR _nested_sysroot_flag_offset LESS _nested_project_offset)
  message(FATAL_ERROR
    "nested GCC canary must audit CMake's resolved SDK and -isysroot flag "
    "after project() enables C++")
endif()
string(REGEX MATCHALL
  "_bioseq_highway_gcc_canary_verify_apple_sdk\\("
  _nested_sdk_verification_matches "${_nested}")
list(LENGTH _nested_sdk_verification_matches
  _nested_sdk_verification_count)
if(NOT _nested_sdk_verification_count EQUAL 2)
  message(FATAL_ERROR
    "nested GCC canary must verify the Apple SDK exactly before and after "
    "project(); found ${_nested_sdk_verification_count}")
endif()

set(_canary_source_root
  "${SOURCE_ROOT}/codegen/namespace_materialization/tests/integration/module_canary/highway_canary")
file(READ "${_canary_source_root}/registry.cppm.in" _registry_source)
foreach(_primary_import IN ITEMS
    "import @SIMD_TARGET_MODULE@;"
    "import @ATTRIBUTED_TARGET_MODULE@;"
    "import @FALLBACK_TARGET_MODULE@;")
  string(FIND "${_registry_source}" "${_primary_import}" _import_offset)
  if(_import_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC registry lost generated target primary ${_primary_import}")
  endif()
endforeach()
string(REGEX MATCHALL "HWY_EXPORT\\(bind_highway_target\\)"
  _export_matches "${_registry_source}")
string(REGEX MATCHALL "HWY_DYNAMIC_DISPATCH\\(bind_highway_target\\)"
  _dispatch_matches "${_registry_source}")
list(LENGTH _export_matches _export_count)
list(LENGTH _dispatch_matches _dispatch_count)
if(NOT _export_count EQUAL 1 OR NOT _dispatch_count EQUAL 1)
  message(FATAL_ERROR
    "Highway GCC canary must own exactly one literal binder dispatch registry")
endif()
string(FIND "${_registry_source}"
  "#define HWY_DISABLED_TARGETS (~(@SIMD_TARGET_TOKEN@ | HWY_EMU128))"
  _registry_runtime_mask)
string(FIND "${_registry_source}"
  "static_assert(HWY_STATIC_TARGET == @SIMD_TARGET_TOKEN@);"
  _registry_static_target_assertion)
string(FIND "${_registry_source}"
  "&@ATTRIBUTED_TARGET_NAMESPACE@::bind_highway_target"
  _attributed_link_anchor)
if(_registry_runtime_mask EQUAL -1
    OR _registry_static_target_assertion EQUAL -1
    OR _attributed_link_anchor EQUAL -1)
  message(FATAL_ERROR
    "Highway registry must exactly match the static target and exclude but "
    "link-anchor the attributed target")
endif()
string(FIND "${_registry_source}" "@ATTRIBUTED_TARGET_TOKEN@"
  _attributed_registry_token)
if(NOT _attributed_registry_token EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC attributed token leaked into the runtime export registry")
endif()
file(READ "${_canary_source_root}/target_kernel.cppm.in" _kernel_source)
foreach(_operation IN ITEMS "hn::LoadU" "hn::Add" "hn::StoreU" "HWY_ATTR")
  string(FIND "${_kernel_source}" "${_operation}" _operation_offset)
  if(_operation_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary kernel lost real Highway operation ${_operation}")
  endif()
endforeach()
file(READ "${_canary_source_root}/target_attribute_shapes.cppm.in"
  _attribute_source)
foreach(_attribute_contract IN ITEMS
    "sizeof(HWY_TARGET_STR) > 1"
    "#undef BIOSEQ_HIGHWAY_LAMBDA_ATTR"
    [=[#if defined(HWY_TARGET_STR) && HWY_HAS_ATTRIBUTE(target)]=]
    [=[#if !HWY_HAS_CPP_ATTRIBUTE(gnu::target)]=]
    "hn::AESRound"
    "hn::SumOfMulQuadAccumulate"
    "attribute_target_only_bf16_dot_product"
    "attribute_function_template"
    "attribute_worker(const std::uint32_t input)"
    "operator()() const"
    "member() const"
    "member_template"
    "highway_attribute_coverage"
    [=[#define BIOSEQ_HIGHWAY_LAMBDA_ATTR [[gnu::target(HWY_TARGET_STR)]]]=]
    [=[#define BIOSEQ_HIGHWAY_LAMBDA_ATTR
#endif]=]
    "[] BIOSEQ_HIGHWAY_LAMBDA_ATTR ("
    "[] BIOSEQ_HIGHWAY_LAMBDA_ATTR {"
    "[]<class p_value> BIOSEQ_HIGHWAY_LAMBDA_ATTR ("
    "requires (sizeof(p_value) > 0) BIOSEQ_HIGHWAY_LAMBDA_ATTR ("
    "const auto value) noexcept(noexcept(value + decltype(value){9}))")
  string(FIND "${_attribute_source}" "${_attribute_contract}"
    _attribute_offset)
  if(_attribute_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC attributed probe lost ${_attribute_contract}")
  endif()
endforeach()
string(REGEX MATCHALL "attribute_target_only_aes"
  _attribute_aes_matches "${_attribute_source}")
string(REGEX MATCHALL "attribute_target_only_bf16_dot_product"
  _attribute_bf16_matches "${_attribute_source}")
list(LENGTH _attribute_aes_matches _attribute_aes_count)
list(LENGTH _attribute_bf16_matches _attribute_bf16_count)
if(NOT _attribute_aes_count EQUAL 1
    OR NOT _attribute_bf16_count EQUAL 1)
  message(FATAL_ERROR
    "Highway target-only AES/BF16 probes must remain defined and uncalled")
endif()
file(GLOB _canary_sources LIST_DIRECTORIES FALSE
  "${_canary_source_root}/*")
foreach(_canary_source IN LISTS _canary_sources)
  file(READ "${_canary_source}" _canary_contents)
  string(FIND "${_canary_contents}" "hwy/foreach_target.h"
    _foreach_target_offset)
  if(NOT _foreach_target_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary must prove fixed target units without foreach_target")
  endif()
endforeach()
foreach(_platform_template IN ITEMS
    registry.cppm.in
    runtime.cpp.in
    target_attribute_shapes.cppm.in
    target_binder.cppm.in
    target_kernel.cppm.in)
  file(READ "${_canary_source_root}/${_platform_template}"
    _platform_template_source)
  string(FIND "${_platform_template_source}"
    "@PLATFORM_HEADER_OVERRIDES@" _platform_override_offset)
  if(_platform_override_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway canary template lost platform-authenticated header policy: "
      "${_platform_template}")
  endif()
endforeach()
file(READ "${_canary_source_root}/runtime.cpp.in" _runtime_source)
foreach(_runtime_contract IN ITEMS
    "HWY_HEADER_ONLY"
    "SupportedTargets()"
    "GetChosenTarget()"
    "does not link libhwy")
  string(FIND "${_runtime_source}" "${_runtime_contract}" _runtime_offset)
  if(_runtime_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runtime lost ABI contract ${_runtime_contract}")
  endif()
endforeach()
string(FIND "${_runtime_source}"
  "#define HWY_DISABLED_TARGETS (~(@SIMD_TARGET_TOKEN@ | HWY_EMU128))"
  _runtime_mask_offset)
string(FIND "${_runtime_source}"
  "return @SIMD_TARGET_TOKEN@ | HWY_EMU128;"
  _supported_mask_offset)
if(_runtime_mask_offset EQUAL -1 OR _supported_mask_offset EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC runtime must remain conservative baseline plus EMU128")
endif()
string(FIND "${_runtime_source}" "@ATTRIBUTED_TARGET_TOKEN@"
  _attributed_runtime_token)
if(NOT _attributed_runtime_token EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC attributed token leaked into SupportedTargets")
endif()

file(READ "${_runner}" _runner_source)
foreach(_sealed_argument IN ITEMS
    "-G Ninja"
    "-DCMAKE_OSX_SYSROOT:PATH="
    "-DCMAKE_TOOLCHAIN_FILE:FILEPATH="
    "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING="
    "-DCMAKE_CXX_FLAGS:STRING="
    "-DCMAKE_PROJECT_INCLUDE_BEFORE:FILEPATH="
    "-DCMAKE_CXX_MODULE_STD:BOOL=OFF"
    "bioseq_verify_gcc16_toolchain_identity"
    "file(REMOVE_RECURSE"
    "/usr/bin/env"
    "-i"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
    [=[DEVELOPER_DIR=${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}]=]
    [=[SDKROOT=${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}]=]
    "BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_PATH"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION"
    "BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST"
    "_bioseq_highway_gcc_canary_verify_apple_sdk"
    "--verbose"
    "bioseq_gcc_canary_closure"
    "_bioseq_highway_gcc_canary_collect_macho_closure"
    "non-system transitive dependency"
    "_bioseq_highway_gcc_audit_macho")
  string(FIND "${_runner_source}" "${_sealed_argument}" _sealed_offset)
  if(_sealed_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runner lost sealed argument: ${_sealed_argument}")
  endif()
endforeach()
string(REGEX MATCHALL
  "_bioseq_highway_gcc_canary_verify_apple_sdk\\("
  _runner_sdk_verification_matches "${_runner_source}")
list(LENGTH _runner_sdk_verification_matches
  _runner_sdk_verification_count)
if(NOT _runner_sdk_verification_count EQUAL 4)
  message(FATAL_ERROR
    "Highway GCC canary runner must verify the Apple SDK exactly before "
    "tree recreation, after configure, immediately before build, and after "
    "build; found "
    "${_runner_sdk_verification_count}")
endif()
_require_ordered_text(
  "Highway GCC canary runner Apple SDK lifecycle"
  "${_runner_source}"
  [=[_bioseq_highway_gcc_canary_verify_apple_sdk(]=]
  [=[file(REMOVE_RECURSE "${_nested_build_normal}")]=]
  [=["-DCMAKE_OSX_SYSROOT:PATH=${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}"]=]
  [=[_bioseq_highway_gcc_canary_verify_apple_sdk(]=]
  [=[-t commands bioseq_gcc_canary_closure]=]
  [=[_bioseq_highway_gcc_canary_verify_apple_sdk(]=]
  [=[--target bioseq_gcc_canary_closure]=]
  [=[_bioseq_highway_gcc_canary_verify_apple_sdk(]=])
foreach(_runner_identity_sdk_field IN ITEMS
    [=["${BIOSEQ_GCC_CANARY_APPLE_DEVELOPER_DIR}\n"]=]
    [=["${BIOSEQ_GCC_CANARY_APPLE_SDK_PATH}\n"]=]
    [=["${BIOSEQ_GCC_CANARY_APPLE_SDK_ROOT}\n"]=]
    [=["${BIOSEQ_GCC_CANARY_APPLE_SDK_VERSION}\n"]=]
    [=["${BIOSEQ_GCC_CANARY_APPLE_SDK_BUILD_VERSION}\n"]=]
    [=["${BIOSEQ_GCC_CANARY_APPLE_SDK_DIGEST}\n"]=])
  string(FIND "${_runner_source}" "${_runner_identity_sdk_field}"
    _runner_identity_sdk_offset)
  if(_runner_identity_sdk_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runner identity omitted Apple SDK field: "
      "${_runner_identity_sdk_field}")
  endif()
endforeach()
foreach(_exact_sysroot_audit_contract IN ITEMS
    [=[separate_arguments(_command_tokens UNIX_COMMAND]=]
    [=[set(_gcc_token_count 0)]=]
    [=[if("${_command_token}" STREQUAL "${BIOSEQ_GCC_CANARY_CXX}")]=]
    [=[if(NOT _gcc_token_count EQUAL 1)]=]
    [=[references the authenticated compiler]=]
    "_sysroot_flag_count"
    [=[if(_command_token STREQUAL "-isysroot")]=]
    [=[math(EXPR _sysroot_value_index]=]
    [=[list(GET _command_tokens ${_sysroot_value_index} _sysroot_value)]=]
    [=["${_sysroot_value}" STREQUAL]=]
    [=[elseif(_command_token MATCHES "^-isysroot.+"]=]
    [=[OR _command_token MATCHES "^--sysroot($|=)"]=]
    "sealed Apple sysroot")
  string(FIND "${_runner_source}" "${_exact_sysroot_audit_contract}"
    _exact_sysroot_audit_offset)
  if(_exact_sysroot_audit_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runner lost exact sysroot command audit text: "
      "${_exact_sysroot_audit_contract}")
  endif()
endforeach()
foreach(_forbidden_runner_text IN ITEMS
    "--unset="
    "_canary_output"
    "checksum=976")
  string(FIND "${_runner_source}" "${_forbidden_runner_text}"
    _forbidden_runner_offset)
  if(NOT _forbidden_runner_offset EQUAL -1)
    message(FATAL_ERROR
      "build-only GCC canary runner retained runtime/denylist text: "
      "${_forbidden_runner_text}")
  endif()
endforeach()
string(FIND "${_nested}" "-fpermissive" _permissive_offset)
string(FIND "${_imported_binder_helper_source}" "-fpermissive"
  _helper_permissive_offset)
if(NOT _permissive_offset EQUAL -1
    OR NOT _helper_permissive_offset EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC canary must not downgrade generated module diagnostics")
endif()

message(STATUS "Highway GCC canary CMake contracts passed")
