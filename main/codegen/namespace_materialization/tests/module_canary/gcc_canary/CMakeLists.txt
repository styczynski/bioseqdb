cmake_minimum_required(VERSION 4.2)

foreach(_required IN ITEMS
    BIOSEQ_GCC_CANARY_CANARY_HELPER
    BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT
    BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT
    BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST
    BIOSEQ_GCC_CANARY_GENERATED_ROOT
    BIOSEQ_GCC_CANARY_PRIMARY
    BIOSEQ_GCC_CANARY_DRIVER
    BIOSEQ_GCC_CANARY_MAIN
    BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER
    BIOSEQ_GCC_CANARY_ROOT
    BIOSEQ_GCC_CANARY_C
    BIOSEQ_GCC_CANARY_CXX
    BIOSEQ_GCC_CANARY_MODULES
    BIOSEQ_GCC_CANARY_VERSION
    BIOSEQ_GCC_CANARY_C_SHA256
    BIOSEQ_GCC_CANARY_CXX_SHA256
    BIOSEQ_GCC_CANARY_MODULES_SHA256
    BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256
    BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "Highway GCC nested canary requires ${_required}")
  endif()
endforeach()
foreach(_file_input IN ITEMS
    BIOSEQ_GCC_CANARY_CANARY_HELPER
    BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER)
  if(NOT IS_ABSOLUTE "${${_file_input}}"
      OR NOT EXISTS "${${_file_input}}"
      OR IS_DIRECTORY "${${_file_input}}")
    message(FATAL_ERROR
      "Highway GCC nested canary requires an absolute regular ${_file_input}")
  endif()
endforeach()
foreach(_directory_input IN ITEMS
    BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT
    BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT
    BIOSEQ_GCC_CANARY_GENERATED_ROOT)
  if(NOT IS_ABSOLUTE "${${_directory_input}}"
      OR NOT IS_DIRECTORY "${${_directory_input}}")
    message(FATAL_ERROR
      "Highway GCC nested canary requires an absolute ${_directory_input}")
  endif()
endforeach()
foreach(_file_input IN ITEMS
    BIOSEQ_GCC_CANARY_PRIMARY
    BIOSEQ_GCC_CANARY_DRIVER
    BIOSEQ_GCC_CANARY_MAIN)
  if(NOT IS_ABSOLUTE "${${_file_input}}"
      OR NOT EXISTS "${${_file_input}}"
      OR IS_DIRECTORY "${${_file_input}}")
    message(FATAL_ERROR
      "Highway GCC nested canary requires an absolute regular ${_file_input}")
  endif()
endforeach()
if(CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
    "Highway GCC nested canary forbids CMAKE_TOOLCHAIN_FILE/vcpkg")
endif()
foreach(_forbidden IN ITEMS
    VCPKG_CHAINLOAD_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET VCPKG_MANIFEST_DIR)
  if(DEFINED ${_forbidden} AND NOT "${${_forbidden}}" STREQUAL "")
    message(FATAL_ERROR
      "Highway GCC nested canary forbids ${_forbidden}")
  endif()
endforeach()

set(CMAKE_CXX_COMPILER "${BIOSEQ_GCC_CANARY_CXX}"
  CACHE FILEPATH "Authenticated Homebrew g++-16" FORCE)
set(CMAKE_CXX_MODULE_STD OFF CACHE BOOL
  "The canary intentionally does not import the standard-library module" FORCE)
set(CMAKE_CXX_SCAN_FOR_MODULES ON CACHE BOOL
  "Scan the isolated generated module closure" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
  "Retain the exact GCC canary compile commands" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)

project(bioseq_highway_gcc_module_canary LANGUAGES CXX)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR "Highway GCC nested canary requires Ninja")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "Highway GCC nested canary supports native Darwin only")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
  message(FATAL_ERROR
    "Highway GCC nested canary requires GNU g++ >= 16.1; found "
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()
file(REAL_PATH "${CMAKE_CXX_COMPILER}" _configured_cxx)
file(REAL_PATH "${BIOSEQ_GCC_CANARY_CXX}" _authenticated_cxx)
if(NOT _configured_cxx STREQUAL _authenticated_cxx)
  message(FATAL_ERROR
    "Highway GCC nested compiler differs from the authenticated g++-16")
endif()
if(CMAKE_CXX_MODULE_STD)
  message(FATAL_ERROR
    "Highway GCC nested canary must keep CMAKE_CXX_MODULE_STD=OFF")
endif()
if(CMAKE_CXX_STDLIB_MODULES_JSON)
  message(FATAL_ERROR
    "Highway GCC nested canary authenticates but does not import std modules")
endif()

include("${BIOSEQ_GCC_CANARY_TOOLCHAIN_HELPER}")
bioseq_verify_gcc16_toolchain_identity(
  ROOT "${BIOSEQ_GCC_CANARY_ROOT}"
  C "${BIOSEQ_GCC_CANARY_C}"
  CXX "${BIOSEQ_GCC_CANARY_CXX}"
  MODULES "${BIOSEQ_GCC_CANARY_MODULES}"
  VERSION "${BIOSEQ_GCC_CANARY_VERSION}"
  C_SHA256 "${BIOSEQ_GCC_CANARY_C_SHA256}"
  CXX_SHA256 "${BIOSEQ_GCC_CANARY_CXX_SHA256}"
  MODULES_SHA256 "${BIOSEQ_GCC_CANARY_MODULES_SHA256}"
  MODULE_SOURCE_CLOSURE_SHA256
    "${BIOSEQ_GCC_CANARY_MODULE_SOURCE_CLOSURE_SHA256}")

include("${BIOSEQ_GCC_CANARY_CANARY_HELPER}")
_bioseq_highway_gcc_canary_driver_closure(
  _observed_driver_closure _gcc_libstdcxx _gcc_libgcc
  "${BIOSEQ_GCC_CANARY_ROOT}" "${BIOSEQ_GCC_CANARY_CXX}")
if(NOT "${_observed_driver_closure}" STREQUAL
    "${BIOSEQ_GCC_CANARY_DRIVER_CLOSURE_SHA256}")
  message(FATAL_ERROR
    "authenticated GCC driver/frontend/header/runtime closure changed")
endif()
file(REAL_PATH "${BIOSEQ_GCC_CANARY_CANARY_SOURCE_ROOT}" _source_root)
file(REAL_PATH "${BIOSEQ_GCC_CANARY_HIGHWAY_INCLUDE_ROOT}" _highway_include)
_bioseq_highway_gcc_canary_header_digest(
  _observed_highway_digest "${_highway_include}")
if(NOT "${_observed_highway_digest}" STREQUAL
    "${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}")
  message(FATAL_ERROR
    "Highway header closure changed after the authenticated outer configure: "
    "expected ${BIOSEQ_GCC_CANARY_HIGHWAY_HEADER_DIGEST}, "
    "observed ${_observed_highway_digest}")
endif()
foreach(_source IN ITEMS
    contract.cppm
    dispatch.cppm
    driver.cppm
    main.cpp
    registry.cppm.in
    runtime.cpp.in
    target_primary.cppm.in
    target_kernel.cppm.in
    target_binder.cppm.in
    target_attribute_shapes.cppm.in)
  if(NOT EXISTS "${_source_root}/${_source}")
    message(FATAL_ERROR
      "Highway GCC canary source closure is missing ${_source_root}/${_source}")
  endif()
endforeach()

# The export/runtime policy remains two deliberately conservative targets. A
# third private target compiles nonempty HWY_ATTR forms and one AES operation,
# but is only link-anchored and never appears in the dispatch table.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(SIMD_TARGET_SLUG neon_without_aes)
  set(SIMD_TARGET_TOKEN HWY_NEON_WITHOUT_AES)
  set(SIMD_TARGET_NAMESPACE N_NEON_WITHOUT_AES)
  set(ATTRIBUTED_TARGET_SLUG neon)
  set(ATTRIBUTED_TARGET_TOKEN HWY_NEON)
  set(ATTRIBUTED_TARGET_NAMESPACE N_NEON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(SIMD_TARGET_SLUG sse2)
  set(SIMD_TARGET_TOKEN HWY_SSE2)
  set(SIMD_TARGET_NAMESPACE N_SSE2)
  set(ATTRIBUTED_TARGET_SLUG sse4)
  set(ATTRIBUTED_TARGET_TOKEN HWY_SSE4)
  set(ATTRIBUTED_TARGET_NAMESPACE N_SSE4)
else()
  message(FATAL_ERROR
    "Highway GCC canary has no conservative Darwin target pair for "
    "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(SIMD_TARGET_MODULE
  "bioseq.highway.gcc_canary.target.${SIMD_TARGET_SLUG}")
set(ATTRIBUTED_TARGET_MODULE
  "bioseq.highway.gcc_canary.target.${ATTRIBUTED_TARGET_SLUG}")
set(FALLBACK_TARGET_SLUG emu128)
set(FALLBACK_TARGET_TOKEN HWY_EMU128)
set(FALLBACK_TARGET_NAMESPACE N_EMU128)
set(FALLBACK_TARGET_MODULE bioseq.highway.gcc_canary.target.emu128)

set(_generated_root "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY
  "${_generated_root}/${SIMD_TARGET_SLUG}"
  "${_generated_root}/${ATTRIBUTED_TARGET_SLUG}"
  "${_generated_root}/${FALLBACK_TARGET_SLUG}")

function(_configure_target
    slug token target_namespace module_name policy_mask attribute_probe)
  set(TARGET_SLUG "${slug}")
  set(TARGET_TOKEN "${token}")
  set(TARGET_NAMESPACE "${target_namespace}")
  set(TARGET_MODULE "${module_name}")
  set(TARGET_POLICY_MASK "${policy_mask}")
  set(ATTRIBUTE_PARTITION_IMPORT "")
  set(ATTRIBUTE_COVERAGE_CHECK "")
  if(attribute_probe)
    set(ATTRIBUTE_PARTITION_IMPORT "import :attribute_shapes;")
    set(ATTRIBUTE_COVERAGE_CHECK
      "  if (HighwayAttributeCoverage(7u) == 0u) {\n    return nullptr;\n  }")
    configure_file(
      "${_source_root}/target_attribute_shapes.cppm.in"
      "${_generated_root}/${slug}/attribute_shapes.cppm"
      @ONLY)
  endif()
  foreach(_kind IN ITEMS primary kernel binder)
    if(_kind STREQUAL "primary")
      set(_output_name index.cppm)
    else()
      set(_output_name "${_kind}.cppm")
    endif()
    configure_file(
      "${_source_root}/target_${_kind}.cppm.in"
      "${_generated_root}/${slug}/${_output_name}"
      @ONLY)
  endforeach()
endfunction()

_configure_target(
  "${SIMD_TARGET_SLUG}"
  "${SIMD_TARGET_TOKEN}"
  "${SIMD_TARGET_NAMESPACE}"
  "${SIMD_TARGET_MODULE}"
  "${SIMD_TARGET_TOKEN} | HWY_EMU128"
  FALSE)
_configure_target(
  "${ATTRIBUTED_TARGET_SLUG}"
  "${ATTRIBUTED_TARGET_TOKEN}"
  "${ATTRIBUTED_TARGET_NAMESPACE}"
  "${ATTRIBUTED_TARGET_MODULE}"
  "${SIMD_TARGET_TOKEN} | ${ATTRIBUTED_TARGET_TOKEN} | HWY_EMU128"
  TRUE)
_configure_target(
  "${FALLBACK_TARGET_SLUG}"
  "${FALLBACK_TARGET_TOKEN}"
  "${FALLBACK_TARGET_NAMESPACE}"
  "${FALLBACK_TARGET_MODULE}"
  "${SIMD_TARGET_TOKEN} | HWY_EMU128"
  FALSE)
configure_file(
  "${_source_root}/registry.cppm.in"
  "${_generated_root}/highway_registry.cppm"
  @ONLY)
configure_file(
  "${_source_root}/runtime.cpp.in"
  "${_generated_root}/runtime.cpp"
  @ONLY)

set(_target_primaries
  "${_generated_root}/${SIMD_TARGET_SLUG}/index.cppm"
  "${_generated_root}/${ATTRIBUTED_TARGET_SLUG}/index.cppm"
  "${_generated_root}/${FALLBACK_TARGET_SLUG}/index.cppm")
set(_target_partitions
  "${_generated_root}/${SIMD_TARGET_SLUG}/kernel.cppm"
  "${_generated_root}/${ATTRIBUTED_TARGET_SLUG}/kernel.cppm"
  "${_generated_root}/${ATTRIBUTED_TARGET_SLUG}/attribute_shapes.cppm"
  "${_generated_root}/${FALLBACK_TARGET_SLUG}/kernel.cppm")
set(_target_binders
  "${_generated_root}/${SIMD_TARGET_SLUG}/binder.cppm"
  "${_generated_root}/${ATTRIBUTED_TARGET_SLUG}/binder.cppm"
  "${_generated_root}/${FALLBACK_TARGET_SLUG}/binder.cppm")
set(_registry "${_generated_root}/highway_registry.cppm")
set(_runtime "${_generated_root}/runtime.cpp")
set_source_files_properties(
  ${_target_primaries}
  ${_target_partitions}
  ${_target_binders}
  "${_registry}"
  "${_runtime}"
  PROPERTIES GENERATED TRUE)

add_executable(bioseq_highway_gcc_module_canary)
target_sources(bioseq_highway_gcc_module_canary
  PRIVATE
    "${_source_root}/main.cpp"
    "${_source_root}/driver.cppm"
    "${_runtime}"
    ${_target_partitions}
    ${_target_binders}
    "${_registry}"
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS "${_source_root}" "${_generated_root}"
    FILES
      "${_source_root}/contract.cppm"
      "${_source_root}/dispatch.cppm"
      ${_target_primaries})
# The fixed-target kernels and registry are private partitions; the binders and
# driver are implementation units. Keep all of them as ordinary private C++
# sources while preserving module dependency scanning, matching production.
set_source_files_properties(
  "${_source_root}/driver.cppm"
  ${_target_partitions}
  ${_target_binders}
  "${_registry}"
  PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
target_include_directories(bioseq_highway_gcc_module_canary
  SYSTEM PRIVATE "${_highway_include}")
set_target_properties(bioseq_highway_gcc_module_canary PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD OFF
  CXX_SCAN_FOR_MODULES ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

# Retain the original namespace-materialization GCC closure beside the real
# Highway boundary so the specialized proof does not regress generic generated
# implementation-unit/module dependency coverage.
set(_generic_generated_sources
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/typed_flat_api.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/typed_flat_selector.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_0.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_1.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_2.cppm"
  "${BIOSEQ_GCC_CANARY_GENERATED_ROOT}/instance_3.cppm")
foreach(_generic_source IN LISTS _generic_generated_sources)
  if(NOT IS_ABSOLUTE "${_generic_source}"
      OR NOT EXISTS "${_generic_source}"
      OR IS_DIRECTORY "${_generic_source}")
    message(FATAL_ERROR
      "generic GCC canary input is missing: ${_generic_source}")
  endif()
endforeach()
get_filename_component(
  _generic_primary_base "${BIOSEQ_GCC_CANARY_PRIMARY}" DIRECTORY)
add_executable(bioseq_namespace_materialization_gcc_module_canary)
target_sources(bioseq_namespace_materialization_gcc_module_canary
  PRIVATE
    "${BIOSEQ_GCC_CANARY_MAIN}"
    ${_generic_generated_sources}
    "${BIOSEQ_GCC_CANARY_DRIVER}"
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS "${_generic_primary_base}"
    FILES "${BIOSEQ_GCC_CANARY_PRIMARY}")
set_source_files_properties(
  ${_generic_generated_sources}
  "${BIOSEQ_GCC_CANARY_DRIVER}"
  PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
set_target_properties(
  bioseq_namespace_materialization_gcc_module_canary PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD OFF
    CXX_SCAN_FOR_MODULES ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)

foreach(_canary_target IN ITEMS
    bioseq_highway_gcc_module_canary
    bioseq_namespace_materialization_gcc_module_canary)
  get_target_property(_canary_link_libraries
    "${_canary_target}" LINK_LIBRARIES)
  if(_canary_link_libraries)
    message(FATAL_ERROR
      "Highway GCC nested canaries must not link the mixed-runtime outer "
      "libhwy, vcpkg, LLVM, Clang-Tooling, or any other explicit C++ library: "
      "${_canary_link_libraries}")
  endif()
endforeach()

add_custom_target(bioseq_gcc_canary_closure
  DEPENDS
    bioseq_highway_gcc_module_canary
    bioseq_namespace_materialization_gcc_module_canary)
