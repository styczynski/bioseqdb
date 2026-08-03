cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/BioseqHighwayCodegen.cmake")

function(_contract_require_equal actual expected message_text)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "Highway probe-identity contract failed: ${message_text}\n"
      "actual='${actual}'\nexpected='${expected}'")
  endif()
endfunction()

if(NOT TEST_ROOT)
  set(TEST_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/highway-probe-identity-contract")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY
  "${TEST_ROOT}/compile-sysroot"
  "${TEST_ROOT}/fallback-sysroot"
  "${TEST_ROOT}/external-toolchain"
  "${TEST_ROOT}/apple-sdk")
file(REAL_PATH "${TEST_ROOT}" TEST_ROOT)

if(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "ambiguous-apple")
  set(APPLE TRUE)
  set(CMAKE_CXX_COMPILER_ID AppleClang)
  set(CMAKE_SYSTEM_PROCESSOR arm64)
  set(CMAKE_OSX_SYSROOT "${TEST_ROOT}/apple-sdk")
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
  _bioseq_highway_configured_probe_identity(_flags _records _digest)
  message(FATAL_ERROR "ambiguous Apple architecture unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "conflicting-definition")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -DHWY_TEST_POLICY=2
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "conflicting policy definition unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "conflicting-undefinition")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -U HWY_TEST_POLICY
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "conflicting policy undefinition unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "ambient-identity-option")
  set(APPLE FALSE)
  set(CMAKE_CXX_COMPILER_ID Clang)
  _bioseq_highway_configure_probe_policy(
    _definitions _options _identity_flags _identity_records _identity_digest
    COMPILE_OPTIONS -isysroot "${TEST_ROOT}/apple-sdk")
  message(FATAL_ERROR "ambient driver identity unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "unsafe-policy-replacement")
  set(APPLE FALSE)
  set(CMAKE_CXX_COMPILER_ID Clang)
  _bioseq_highway_configure_probe_policy(
    _definitions _options _identity_flags _identity_records _identity_digest
    DEFINITIONS "HWY_TEST_POLICY=1/*source-splice*/")
  message(FATAL_ERROR "unsafe policy replacement unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "ambient-highway-selector")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -DHWY_TARGET=HWY_SCALAR
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "ambient Highway selector unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "ambient-bioseq-policy")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -UBIOSEQ_HIGHWAY_UNSEALED
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "ambient Bioseq policy unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "platform-disagreement")
  set(APPLE TRUE)
  set(CMAKE_CXX_COMPILER_TARGET x86_64-apple-darwin)
  set(CMAKE_OSX_ARCHITECTURES arm64)
  set(CMAKE_SYSTEM_PROCESSOR arm64)
  _bioseq_highway_resolve_platform(_platform)
  message(FATAL_ERROR "conflicting authenticated platforms unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "forced-preinclude")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -include "${TEST_ROOT}/forced.h"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "forced preinclude unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "forwarded-preinclude")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -Xclang -include-pch "${TEST_ROOT}/forced.pch"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "forwarded forced preinclude unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "forced-imacros")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS "-Wp,-imacros,${TEST_ROOT}/forced.h"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "forced imacros input unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "opaque-response-file")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS "@${TEST_ROOT}/opaque.rsp"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "opaque response-file argument unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "opaque-clang-config")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS "--config=${TEST_ROOT}/clang.cfg"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "opaque Clang config unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "opaque-clang-system-config-dir")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS
      "--config-system-dir=${TEST_ROOT}/clang-system-configs"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "opaque Clang system config directory unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "opaque-clang-user-config-dir")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS
      --config-user-dir "${TEST_ROOT}/clang-user-configs"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "opaque Clang user config directory unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "opaque-gcc-specs")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -specs "${TEST_ROOT}/compiler.specs"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "opaque GCC specs unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "lowlevel-target-forwarding")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -Xclang -triple -Xclang aarch64-unknown-linux-gnu
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "low-level target forwarding unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "wp-identity-forwarding")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS
      "-Wp,--sysroot=${TEST_ROOT}/fallback-sysroot"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "preprocessor identity forwarding unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "multi-config")
  set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
  _bioseq_highway_require_single_config_probe()
  message(FATAL_ERROR "multi-config target probe unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "machine-option")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -march=armv8-a
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "target-shaping machine option unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "machine-option-shell")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILE_OPTIONS "SHELL:-mcpu cortex-a76"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "SHELL target machine option unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "undef-builtins")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -undef
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "compiler builtin undefinition unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-search-paired")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -I "${TEST_ROOT}/shadow-include"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "paired header-search override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-search-joined")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS "-isystem${TEST_ROOT}/shadow-system"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "joined header-search override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-vfs-shell")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILE_OPTIONS
      "SHELL:-ivfsoverlay ${TEST_ROOT}/shadow-overlay.yaml"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "SHELL VFS overlay unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-vfs-driver-alias")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -vfsoverlay "${TEST_ROOT}/shadow-overlay.yaml"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "driver VFS overlay alias unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-cxx-isystem")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -cxx-isystem "${TEST_ROOT}/shadow-cxx-system"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "C++ system search override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-stdlib-isystem")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILE_OPTIONS
      "SHELL:-stdlib++-isystem ${TEST_ROOT}/shadow-stdlib-system"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "stdlib system search override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-iwithsysroot")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -iwithsysroot "${TEST_ROOT}/shadow-sysroot-search"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "sysroot-relative search override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-resource-dir")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS "-resource-dir=${TEST_ROOT}/shadow-resource-dir"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "compiler resource directory override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-nostdinc")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -nostdinc
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "standard include suppression unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-nostdincxx")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILE_OPTIONS "SHELL:-nostdinc++"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "C++ include suppression unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-nostdlibinc")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -nostdlibinc
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "stdlib include suppression unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-nobuiltininc")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILER_ARGUMENTS -nobuiltininc
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "builtin include suppression unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "header-ibuiltininc")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    COMPILE_OPTIONS -ibuiltininc
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "builtin include state override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "framework-search")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS "-F${TEST_ROOT}/shadow-frameworks"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "framework search override unexpectedly succeeded")
elseif(PROBE_IDENTITY_CONTRACT_CASE STREQUAL "forced-pth")
  _bioseq_highway_prepare_direct_probe_policy(_arguments
    CONFIGURED_FLAGS -include-pth "${TEST_ROOT}/forced.pth"
    DEFINITIONS HWY_TEST_POLICY=1)
  message(FATAL_ERROR "forced PTH input unexpectedly succeeded")
endif()

set(APPLE FALSE)
set(CMAKE_CXX_COMPILER_ID Clang)
set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-gnu)
set(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
  "${TEST_ROOT}/external-toolchain")
set(CMAKE_SYSROOT_COMPILE "${TEST_ROOT}/compile-sysroot")
set(CMAKE_SYSROOT "${TEST_ROOT}/fallback-sysroot")
unset(CMAKE_OSX_SYSROOT)
unset(CMAKE_OSX_ARCHITECTURES)
unset(CMAKE_OSX_DEPLOYMENT_TARGET)
unset(CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG)
_bioseq_highway_configured_probe_identity(
  _cross_flags _cross_records _cross_digest)
_contract_require_equal(
  "${_cross_flags}"
  "--target=aarch64-unknown-linux-gnu;--gcc-toolchain=${TEST_ROOT}/external-toolchain;--sysroot=${TEST_ROOT}/compile-sysroot"
  "Clang cross identity omitted or reordered a configured driver flag")
_contract_require_equal(
  "${_cross_records}"
  "target=--target=aarch64-unknown-linux-gnu;external-toolchain=--gcc-toolchain=${TEST_ROOT}/external-toolchain;sysroot=--sysroot=${TEST_ROOT}/compile-sysroot"
  "cross identity provenance does not match normalized flags")
set(CMAKE_SYSTEM_PROCESSOR x86_64)
_bioseq_highway_resolve_platform(_cross_platform)
_contract_require_equal(
  "${_cross_platform}" aarch64
  "platform slug followed host processor instead of compiler target triple")
_bioseq_highway_configure_probe_policy(
  _policy_definitions _policy_options
  _policy_identity_flags _policy_identity_records _policy_identity_digest
  DEFINITIONS HWY_TEST_POLICY HWY_TEST_POLICY=1
  COMPILE_OPTIONS -Wno-unused-command-line-argument)
_contract_require_equal(
  "${_policy_identity_flags}" "${_cross_flags}"
  "policy configuration lost direct-probe identity flags")
_contract_require_equal(
  "${_policy_options}"
  "-Wno-unused-command-line-argument;${_cross_flags}"
  "policy target options do not end in normalized probe identity order")
list(FIND _policy_definitions
  "BIOSEQ_HIGHWAY_PROBE_COMPILATION_IDENTITY=${_cross_digest}"
  _identity_definition_index)
if(_identity_definition_index EQUAL -1)
  message(FATAL_ERROR
    "probe identity digest did not reach authenticated policy definitions")
endif()
_contract_require_equal(
  "${_policy_identity_digest}" "${_cross_digest}"
  "policy identity digest differs from direct-probe identity")
_bioseq_highway_render_policy_block(
  _policy_block ${_policy_definitions})
string(CONCAT _expected_policy_block
  "#define BIOSEQ_HIGHWAY_PROBE_COMPILATION_IDENTITY ${_cross_digest}\n"
  "#define HWY_TEST_POLICY 1\n")
_contract_require_equal(
  "${_policy_block}" "${_expected_policy_block}"
  "canonical policy renderer changed the exact authenticated bytes")
string(SHA256 _policy_block_digest "${_policy_block}")
_bioseq_highway_render_target_probe_source(
  _rendered_probe "${_policy_block}")
string(FIND "${_rendered_probe}" "${_policy_block}" _probe_policy_offset)
string(FIND "${_rendered_probe}" "#include <hwy/highway.h>"
  _probe_highway_include_offset)
if(_probe_policy_offset EQUAL -1
    OR _probe_highway_include_offset EQUAL -1
    OR _probe_policy_offset GREATER _probe_highway_include_offset)
  message(FATAL_ERROR
    "direct probe does not embed the canonical block before Highway headers")
endif()
_bioseq_highway_prepare_direct_probe_policy(_direct_probe_policy_arguments
  LABEL "scripted direct Highway probe"
  COMPILER_ARGUMENTS --driver-mode=g++
  CONFIGURED_FLAGS -DHWY_TEST_POLICY=1 -Wconfigured-probe-option
  COMPILE_OPTIONS ${_policy_options}
  IDENTITY_FLAGS ${_policy_identity_flags}
  DEFINITIONS ${_policy_definitions})
_contract_require_equal(
  "${_direct_probe_policy_arguments}"
  "-DHWY_TEST_POLICY=1;-Wconfigured-probe-option;${_policy_options}"
  "normalized identity flags did not reach the direct probe argument suffix")
_bioseq_highway_render_probe_identity_provenance(
  _cross_provenance "${_cross_digest}" ${_cross_records})
string(CONCAT _expected_cross_provenance
  "bioseq-highway-probe-compilation-identity-v1\n"
  "identity\t${_cross_digest}\n"
  "flag\t0\ttarget=--target=aarch64-unknown-linux-gnu\n"
  "flag\t1\texternal-toolchain=--gcc-toolchain=${TEST_ROOT}/external-toolchain\n"
  "flag\t2\tsysroot=--sysroot=${TEST_ROOT}/compile-sysroot\n")
_contract_require_equal(
  "${_cross_provenance}" "${_expected_cross_provenance}"
  "provenance does not serialize exact normalized identity order")
if(NOT _cross_digest MATCHES "^[0-9a-f][0-9a-f]+$")
  message(FATAL_ERROR "cross identity digest is not lowercase hexadecimal")
endif()
string(LENGTH "${_cross_digest}" _cross_digest_length)
if(NOT _cross_digest_length EQUAL 64)
  message(FATAL_ERROR "cross identity digest is not SHA-256 sized")
endif()

set(CMAKE_CXX_COMPILER_TARGET riscv64-unknown-linux-gnu)
_bioseq_highway_configured_probe_identity(
  _changed_flags _changed_records _changed_digest)
if(_changed_digest STREQUAL _cross_digest)
  message(FATAL_ERROR "target-triple mutation did not change identity digest")
endif()
_bioseq_highway_configure_probe_policy(
  _changed_definitions _changed_options
  _changed_identity_flags _changed_identity_records _changed_identity_digest
  DEFINITIONS HWY_TEST_POLICY=1)
_bioseq_highway_render_policy_block(
  _changed_policy_block ${_changed_definitions})
string(SHA256 _changed_policy_block_digest "${_changed_policy_block}")
if(_changed_policy_block_digest STREQUAL _policy_block_digest)
  message(FATAL_ERROR
    "target-triple mutation did not change the authenticated policy digest")
endif()

set(APPLE TRUE)
set(CMAKE_CXX_COMPILER_ID AppleClang)
set(CMAKE_CXX_COMPILER_TARGET arm64-apple-darwin)
unset(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN)
unset(CMAKE_SYSROOT_COMPILE)
unset(CMAKE_SYSROOT)
set(CMAKE_OSX_SYSROOT "${TEST_ROOT}/apple-sdk")
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(_CMAKE_OSX_SYSROOT_PATH "${TEST_ROOT}/apple-sdk")
unset(_CMAKE_APPLE_ARCHS_DEFAULT)
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
set(CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG "-mmacosx-version-min=")
_bioseq_highway_configured_probe_identity(
  _apple_flags _apple_records _apple_digest)
_contract_require_equal(
  "${_apple_flags}"
  "--target=arm64-apple-darwin;-isysroot;${TEST_ROOT}/apple-sdk;-arch;arm64;-mmacosx-version-min=14.0"
  "Apple identity omitted sysroot, architecture, or deployment target")
_contract_require_equal(
  "${_apple_records}"
  "target=--target=arm64-apple-darwin;apple-sysroot=-isysroot|${TEST_ROOT}/apple-sdk;apple-architecture=-arch|arm64;apple-deployment=-mmacosx-version-min=14.0"
  "Apple provenance does not match normalized flags")
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.1)
_bioseq_highway_configured_probe_identity(
  _changed_deployment_flags _changed_deployment_records
  _changed_deployment_digest)
if(_changed_deployment_digest STREQUAL _apple_digest)
  message(FATAL_ERROR
    "Apple deployment-target mutation did not change identity digest")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)

set(CMAKE_OSX_SYSROOT macosx)
set(CMAKE_OSX_ARCHITECTURES "")
set(_CMAKE_APPLE_ARCHS_DEFAULT arm64)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
_bioseq_highway_configured_probe_identity(
  _sdk_name_flags _sdk_name_records _sdk_name_digest)
_contract_require_equal(
  "${_sdk_name_flags}"
  "--target=arm64-apple-darwin;-isysroot;${TEST_ROOT}/apple-sdk;-arch;arm64;-mmacosx-version-min=14.0"
  "SDK-name resolution or CMake's default Apple arch was not preserved")

set(CMAKE_OSX_SYSROOT "")
_bioseq_highway_configured_probe_identity(
  _implicit_sdk_flags _implicit_sdk_records _implicit_sdk_digest)
_contract_require_equal(
  "${_implicit_sdk_flags}" "${_sdk_name_flags}"
  "empty CMAKE_OSX_SYSROOT lost CMake's effective resolved SDK path")
_bioseq_highway_resolve_platform(_authenticated_apple_platform)
_contract_require_equal(
  "${_authenticated_apple_platform}" aarch64
  "platform slug followed host processor instead of authenticated Apple arch")

unset(CMAKE_CXX_COMPILER_TARGET)
set(CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG
  "--target=<ARCH>-apple-xros<VERSION_MIN>")
_bioseq_highway_configured_probe_identity(
  _placeholder_flags _placeholder_records _placeholder_digest)
_contract_require_equal(
  "${_placeholder_flags}"
  "-isysroot;${TEST_ROOT}/apple-sdk;-arch;arm64;--target=arm64-apple-xros14.0"
  "Apple deployment flag placeholders were not reconstructed exactly")

function(_contract_expect_child_failure case_name diagnostic_pattern)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${TEST_ROOT}/${case_name}"
      "-DPROBE_IDENTITY_CONTRACT_CASE=${case_name}"
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _status
    ERROR_VARIABLE _error)
  if(_status EQUAL 0)
    message(FATAL_ERROR
      "contract child '${case_name}' unexpectedly succeeded")
  endif()
  if(NOT _error MATCHES "${diagnostic_pattern}")
    message(FATAL_ERROR
      "contract child '${case_name}' did not fail closed as expected:\n"
      "${_error}")
  endif()
endfunction()

_contract_expect_child_failure(
  ambiguous-apple "ambiguous multi-architecture")
_contract_expect_child_failure(
  conflicting-definition "canonical policy block defines")
_contract_expect_child_failure(
  conflicting-undefinition "undefines canonical policy macro")
_contract_expect_child_failure(
  ambient-identity-option "dedicated driver identity option")
_contract_expect_child_failure(
  unsafe-policy-replacement "unsafe canonical source replacement")
_contract_expect_child_failure(
  ambient-highway-selector "unregistered Highway policy macro HWY_TARGET")
_contract_expect_child_failure(
  ambient-bioseq-policy
  "unregistered Highway policy macro BIOSEQ_HIGHWAY_UNSEALED")
_contract_expect_child_failure(
  platform-disagreement "disagrees with effective Apple architecture")
_contract_expect_child_failure(
  forced-preinclude "unauthenticated forced preprocessor input")
_contract_expect_child_failure(
  forwarded-preinclude "unauthenticated compiler forwarding option")
_contract_expect_child_failure(
  forced-imacros "unauthenticated compiler forwarding option")
_contract_expect_child_failure(
  opaque-response-file "opaque compiler response-file argument")
_contract_expect_child_failure(
  opaque-clang-config "opaque compiler configuration option")
_contract_expect_child_failure(
  opaque-clang-system-config-dir "opaque compiler configuration option")
_contract_expect_child_failure(
  opaque-clang-user-config-dir "opaque compiler configuration option")
_contract_expect_child_failure(
  opaque-gcc-specs "opaque compiler configuration option")
_contract_expect_child_failure(
  lowlevel-target-forwarding "unauthenticated compiler forwarding option")
_contract_expect_child_failure(
  wp-identity-forwarding "unauthenticated compiler forwarding option")
_contract_expect_child_failure(
  multi-config "does not support multi-config generators")
_contract_expect_child_failure(
  machine-option "unauthenticated target-shaping machine option")
_contract_expect_child_failure(
  machine-option-shell "unauthenticated target-shaping machine option")
_contract_expect_child_failure(
  undef-builtins "suppresses authenticated compiler predefined macros")
_contract_expect_child_failure(
  header-search-paired "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-search-joined "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-vfs-shell "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-vfs-driver-alias "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-cxx-isystem "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-stdlib-isystem "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-iwithsysroot "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-resource-dir "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-nostdinc "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-nostdincxx "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-nostdlibinc "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-nobuiltininc "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  header-ibuiltininc "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  framework-search "unauthenticated header-search/VFS option")
_contract_expect_child_failure(
  forced-pth "unauthenticated forced preprocessor input")

message(STATUS "Highway probe-identity script contract passed")
