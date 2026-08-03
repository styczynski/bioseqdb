include_guard(GLOBAL)

function(bioseq_add_highway_imported_binder_canary)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "TARGET;SOURCE_ROOT;HIGHWAY_INCLUDE_ROOT;GENERATED_ROOT"
    "")
  foreach(_required IN ITEMS
      TARGET SOURCE_ROOT HIGHWAY_INCLUDE_ROOT GENERATED_ROOT)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      message(FATAL_ERROR
        "bioseq_add_highway_imported_binder_canary requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "bioseq_add_highway_imported_binder_canary received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(TARGET "${ARG_TARGET}")
    message(FATAL_ERROR
      "Highway imported-binder canary target already exists: ${ARG_TARGET}")
  endif()
  foreach(_directory IN ITEMS SOURCE_ROOT HIGHWAY_INCLUDE_ROOT)
    if(NOT IS_ABSOLUTE "${ARG_${_directory}}"
        OR NOT IS_DIRECTORY "${ARG_${_directory}}")
      message(FATAL_ERROR
        "Highway imported-binder canary ${_directory} must be an absolute "
        "directory: ${ARG_${_directory}}")
    endif()
  endforeach()
  if(NOT IS_ABSOLUTE "${ARG_GENERATED_ROOT}")
    message(FATAL_ERROR
      "Highway imported-binder canary GENERATED_ROOT must be absolute")
  endif()
  file(REAL_PATH "${ARG_SOURCE_ROOT}" _source_root)
  file(REAL_PATH "${ARG_HIGHWAY_INCLUDE_ROOT}" _highway_include)
  cmake_path(
    NORMAL_PATH ARG_GENERATED_ROOT OUTPUT_VARIABLE _generated_root)
  if(NOT EXISTS "${_highway_include}/hwy/highway.h"
      OR NOT EXISTS "${_highway_include}/hwy/targets.h")
    message(FATAL_ERROR
      "Highway imported-binder canary requires a complete Highway include "
      "root: ${_highway_include}")
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
    if(NOT EXISTS "${_source_root}/${_source}"
        OR IS_DIRECTORY "${_source_root}/${_source}")
      message(FATAL_ERROR
        "Highway imported-binder source closure is missing "
        "${_source_root}/${_source}")
    endif()
  endforeach()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR
      "Highway imported-binder canary requires GNU or Clang-family C++; "
      "found ${CMAKE_CXX_COMPILER_ID}")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_HEADER_OVERRIDES
      "#define TOOLCHAIN_MISS_SYS_AUXV_H 1\n#define TOOLCHAIN_MISS_ASM_HWCAP_H 1")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_HEADER_OVERRIDES "")
  else()
    message(FATAL_ERROR
      "Highway imported-binder canary supports fail-closed native Darwin "
      "and Linux builds only; found ${CMAKE_SYSTEM_NAME}")
  endif()

  set(_target_processor "${CMAKE_SYSTEM_PROCESSOR}")
  if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_apple_architectures ${CMAKE_OSX_ARCHITECTURES})
    list(REMOVE_DUPLICATES _apple_architectures)
    list(LENGTH _apple_architectures _apple_architecture_count)
    if(NOT _apple_architecture_count EQUAL 1)
      message(FATAL_ERROR
        "Highway imported-binder canary cannot represent a multi-architecture "
        "CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}'")
    endif()
    list(GET _apple_architectures 0 _target_processor)
  endif()

  # Runtime dispatch remains a conservative native baseline plus EMU128. The
  # third target compiles real target-attributed callable shapes and a
  # target-distinguishing vector operation, but is only link-anchored and
  # cannot enter the exported dispatch table.
  if(_target_processor MATCHES "^(arm64|aarch64)$")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      set(SIMD_TARGET_SLUG neon_without_aes)
      set(SIMD_TARGET_TOKEN HWY_NEON_WITHOUT_AES)
      set(SIMD_TARGET_NAMESPACE N_NEON_WITHOUT_AES)
      set(ATTRIBUTED_TARGET_SLUG neon)
      set(ATTRIBUTED_TARGET_TOKEN HWY_NEON)
      set(ATTRIBUTED_TARGET_NAMESPACE N_NEON)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      # Apple arm64 Clang's production Highway manifest has NEON (AES) as its
      # exact static target; BF16 is a distinct compile-only attributed target.
      set(SIMD_TARGET_SLUG neon)
      set(SIMD_TARGET_TOKEN HWY_NEON)
      set(SIMD_TARGET_NAMESPACE N_NEON)
      set(ATTRIBUTED_TARGET_SLUG neon_bf16)
      set(ATTRIBUTED_TARGET_TOKEN HWY_NEON_BF16)
      set(ATTRIBUTED_TARGET_NAMESPACE N_NEON_BF16)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Baseline aarch64 Linux Clang does not imply AES. The exact static
      # assertion below deliberately fails if the selected toolchain differs.
      set(SIMD_TARGET_SLUG neon_without_aes)
      set(SIMD_TARGET_TOKEN HWY_NEON_WITHOUT_AES)
      set(SIMD_TARGET_NAMESPACE N_NEON_WITHOUT_AES)
      set(ATTRIBUTED_TARGET_SLUG neon)
      set(ATTRIBUTED_TARGET_TOKEN HWY_NEON)
      set(ATTRIBUTED_TARGET_NAMESPACE N_NEON)
    else()
      message(FATAL_ERROR
        "Highway imported-binder canary has no arm64 Clang target policy for "
        "${CMAKE_SYSTEM_NAME}")
    endif()
  elseif(_target_processor MATCHES "^(x86_64|amd64|AMD64)$")
    set(SIMD_TARGET_SLUG sse2)
    set(SIMD_TARGET_TOKEN HWY_SSE2)
    set(SIMD_TARGET_NAMESPACE N_SSE2)
    set(ATTRIBUTED_TARGET_SLUG sse4)
    set(ATTRIBUTED_TARGET_TOKEN HWY_SSE4)
    set(ATTRIBUTED_TARGET_NAMESPACE N_SSE4)
  else()
    message(FATAL_ERROR
      "Highway imported-binder canary has no fail-closed native target pair "
      "for ${_target_processor}")
  endif()
  set(SIMD_TARGET_MODULE
    "bioseq.highway.gcc_canary.target.${SIMD_TARGET_SLUG}")
  set(ATTRIBUTED_TARGET_MODULE
    "bioseq.highway.gcc_canary.target.${ATTRIBUTED_TARGET_SLUG}")
  set(FALLBACK_TARGET_SLUG emu128)
  set(FALLBACK_TARGET_TOKEN HWY_EMU128)
  set(FALLBACK_TARGET_NAMESPACE N_EMU128)
  set(FALLBACK_TARGET_MODULE bioseq.highway.gcc_canary.target.emu128)

  file(MAKE_DIRECTORY
    "${_generated_root}/${SIMD_TARGET_SLUG}"
    "${_generated_root}/${ATTRIBUTED_TARGET_SLUG}"
    "${_generated_root}/${FALLBACK_TARGET_SLUG}")

  function(_bioseq_configure_highway_imported_binder_target
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
      if(TARGET_TOKEN STREQUAL "HWY_NEON_BF16")
        set(TARGET_DISTINGUISHING_PROBE_KIND 2)
      elseif(TARGET_TOKEN STREQUAL "HWY_NEON"
          OR TARGET_TOKEN STREQUAL "HWY_SSE4")
        set(TARGET_DISTINGUISHING_PROBE_KIND 1)
      else()
        message(FATAL_ERROR
          "Highway imported-binder canary has no distinguishing operation "
          "for attributed target ${TARGET_TOKEN}")
      endif()
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

  _bioseq_configure_highway_imported_binder_target(
    "${SIMD_TARGET_SLUG}"
    "${SIMD_TARGET_TOKEN}"
    "${SIMD_TARGET_NAMESPACE}"
    "${SIMD_TARGET_MODULE}"
    "${SIMD_TARGET_TOKEN} | HWY_EMU128"
    FALSE)
  _bioseq_configure_highway_imported_binder_target(
    "${ATTRIBUTED_TARGET_SLUG}"
    "${ATTRIBUTED_TARGET_TOKEN}"
    "${ATTRIBUTED_TARGET_NAMESPACE}"
    "${ATTRIBUTED_TARGET_MODULE}"
    "${SIMD_TARGET_TOKEN} | ${ATTRIBUTED_TARGET_TOKEN} | HWY_EMU128"
    TRUE)
  _bioseq_configure_highway_imported_binder_target(
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

  add_executable("${ARG_TARGET}")
  target_sources("${ARG_TARGET}"
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
  # Fixed-target kernels and the sole registry are private partitions; binders
  # and the driver are implementation units imported through the public module.
  set_source_files_properties(
    "${_source_root}/driver.cppm"
    ${_target_partitions}
    ${_target_binders}
    "${_registry}"
    PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
  target_include_directories("${ARG_TARGET}"
    SYSTEM PRIVATE "${_highway_include}")
  set_target_properties("${ARG_TARGET}" PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD OFF
    CXX_SCAN_FOR_MODULES ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)

  get_target_property(_canary_link_libraries "${ARG_TARGET}" LINK_LIBRARIES)
  if(_canary_link_libraries)
    message(FATAL_ERROR
      "Highway imported-binder canary must remain header-only and cannot "
      "link hwy or another explicit C++ runtime library: "
      "${_canary_link_libraries}")
  endif()
endfunction()
