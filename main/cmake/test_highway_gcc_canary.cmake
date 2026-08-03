cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED TEST_ROOT OR "${TEST_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT DEFINED SOURCE_ROOT OR "${SOURCE_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

set(_canary_module "${SOURCE_ROOT}/cmake/BioseqHighwayGccCanary.cmake")
set(_toolchain_helper "${SOURCE_ROOT}/cmake/BioseqGccToolchain.cmake")
set(_runner "${SOURCE_ROOT}/cmake/RunBioseqHighwayGccCanary.cmake")
set(_nested_project
  "${SOURCE_ROOT}/codegen/namespace_materialization/tests/module_canary/gcc_canary/CMakeLists.txt")
foreach(_checked IN ITEMS
    _canary_module _toolchain_helper _runner _nested_project)
  if(NOT EXISTS "${${_checked}}")
    message(FATAL_ERROR "missing Highway GCC canary contract file: ${${_checked}}")
  endif()
endforeach()
file(READ "${_canary_module}" _canary_module_source)
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
    "-dumpspecs"
    "libstdc++.dylib"
    "libstdc++.a"
    "libgcc_s.1.1.dylib"
    "libgcc.a"
    "libgcc_eh.a"
    "include/c++/*")
  string(FIND "${_canary_module_source}" "${_driver_closure_contract}"
    _driver_closure_offset)
  if(_driver_closure_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary lost driver closure evidence "
      "${_driver_closure_contract}")
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

file(READ "${_nested_project}" _nested)
foreach(_required_text IN ITEMS
    "CMAKE_CXX_MODULE_STD OFF"
    "CXX_MODULE_STD OFF"
    "BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT"
    "BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST"
    "HWY_NEON_WITHOUT_AES"
    "HWY_NEON"
    "HWY_SSE2"
    "HWY_SSE4"
    "HWY_EMU128"
    "target_attribute_shapes.cppm.in"
    "attribute_shapes.cppm"
    "typed_flat_api.cppm"
    "instance_3.cppm"
    "bioseq_namespace_materialization_gcc_module_canary"
    "bioseq_gcc_canary_closure"
    "highway_registry.cppm"
    "runtime.cpp"
    "BASE_DIRS"
    "CXX_SCAN_FOR_MODULES ON")
  string(FIND "${_nested}" "${_required_text}" _required_offset)
  if(_required_offset EQUAL -1)
    message(FATAL_ERROR
      "nested GCC canary lost required closure text: ${_required_text}")
  endif()
endforeach()
foreach(_target_policy IN ITEMS
    [=["${SIMD_TARGET_TOKEN} | HWY_EMU128"]=]
    [=["${SIMD_TARGET_TOKEN} | ${ATTRIBUTED_TARGET_TOKEN} | HWY_EMU128"]=])
  string(FIND "${_nested}" "${_target_policy}" _target_policy_offset)
  if(_target_policy_offset EQUAL -1)
    message(FATAL_ERROR
      "nested GCC canary lost exact target policy ${_target_policy}")
  endif()
endforeach()
string(FIND "${_nested}" "find_package(" _find_package_offset)
string(FIND "${_nested}" "target_link_libraries(" _link_libraries_offset)
if(NOT _find_package_offset EQUAL -1
    OR NOT _link_libraries_offset EQUAL -1)
  message(FATAL_ERROR
    "nested GCC canary must have no package discovery or explicit libraries")
endif()

set(_canary_source_root
  "${SOURCE_ROOT}/codegen/namespace_materialization/tests/module_canary/highway_canary")
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
string(REGEX MATCHALL "HWY_EXPORT\\(BindHighwayTarget\\)"
  _export_matches "${_registry_source}")
string(REGEX MATCHALL "HWY_DYNAMIC_DISPATCH\\(BindHighwayTarget\\)"
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
  "&@ATTRIBUTED_TARGET_NAMESPACE@::BindHighwayTarget"
  _attributed_link_anchor)
if(_registry_runtime_mask EQUAL -1 OR _attributed_link_anchor EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC registry must exclude but link-anchor the attributed target")
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
    "hn::AESRound"
    "AttributeFunctionTemplate"
    "AttributeWorker(const std::uint32_t input)"
    "operator()() const"
    "member() const"
    "member_template"
    "HighwayAttributeCoverage"
    ") HWY_ATTR constexpr"
    "[] HWY_ATTR"
    "[]<class Value>(const Value value) HWY_ATTR")
  string(FIND "${_attribute_source}" "${_attribute_contract}"
    _attribute_offset)
  if(_attribute_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC attributed probe lost ${_attribute_contract}")
  endif()
endforeach()
string(REGEX MATCHALL "AttributeTargetOnlyAES"
  _attribute_aes_matches "${_attribute_source}")
list(LENGTH _attribute_aes_matches _attribute_aes_count)
if(NOT _attribute_aes_count EQUAL 1)
  message(FATAL_ERROR
    "Highway GCC target-only AES probe must remain defined and uncalled")
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
file(READ "${_canary_source_root}/runtime.cpp.in" _runtime_source)
foreach(_runtime_contract IN ITEMS
    "HWY_HEADER_ONLY"
    "SupportedTargets()"
    "GetChosenTarget()"
    "does not link the outer Clang/libc++ libhwy.a")
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
    "--verbose"
    "bioseq_gcc_canary_closure"
    "/usr/bin/otool -L"
    "libc"
    "libhwy"
    "unexpected non-system dependency"
    "_bioseq_highway_gcc_audit_macho")
  string(FIND "${_runner_source}" "${_sealed_argument}" _sealed_offset)
  if(_sealed_offset EQUAL -1)
    message(FATAL_ERROR
      "Highway GCC canary runner lost sealed argument: ${_sealed_argument}")
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
if(NOT _permissive_offset EQUAL -1)
  message(FATAL_ERROR
    "Highway GCC canary must not downgrade generated module diagnostics")
endif()

message(STATUS "Highway GCC canary CMake contracts passed")
