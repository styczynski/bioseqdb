# Builder-owned host-native load qualification for both production DSOs.

include_guard(GLOBAL)

function(bioseq_add_production_dso_qualification)
  set(_options)
  set(_one_value_args TARGET BIOSEQ_TARGET CACHE_TARGET)
  cmake_parse_arguments(
    PARSE_ARGV 0
    BIOSEQ_DSO_QUALIFY
    "${_options}"
    "${_one_value_args}"
    "")

  if(BIOSEQ_DSO_QUALIFY_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "bioseq_add_production_dso_qualification: unknown arguments: "
      "${BIOSEQ_DSO_QUALIFY_UNPARSED_ARGUMENTS}")
  endif()
  foreach(_required IN ITEMS TARGET BIOSEQ_TARGET CACHE_TARGET)
    if(NOT BIOSEQ_DSO_QUALIFY_${_required})
      message(FATAL_ERROR
        "bioseq_add_production_dso_qualification: ${_required} is required")
    endif()
  endforeach()
  if(TARGET "${BIOSEQ_DSO_QUALIFY_TARGET}")
    message(FATAL_ERROR
      "production DSO qualification target already exists: "
      "${BIOSEQ_DSO_QUALIFY_TARGET}")
  endif()
  foreach(_extension_target IN ITEMS
      "${BIOSEQ_DSO_QUALIFY_BIOSEQ_TARGET}"
      "${BIOSEQ_DSO_QUALIFY_CACHE_TARGET}")
    if(NOT TARGET "${_extension_target}")
      message(FATAL_ERROR
        "production DSO qualification requires target ${_extension_target}")
    endif()
  endforeach()
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
      "production DSO qualification requires native platform tools")
  endif()
  if(CMAKE_CONFIGURATION_TYPES OR NOT CMAKE_BUILD_TYPE)
    message(FATAL_ERROR
      "production DSO qualification requires one explicit CMAKE_BUILD_TYPE")
  endif()
  foreach(_language IN ITEMS C CXX)
    foreach(_field IN ITEMS COMPILER COMPILER_ID COMPILER_VERSION)
      if(NOT CMAKE_${_language}_${_field})
        message(FATAL_ERROR
          "production DSO qualification requires an identified ${_language} "
          "compiler; CMAKE_${_language}_${_field} is empty")
      endif()
    endforeach()
  endforeach()

  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_program(_bioseq_dso_qualification_tool NAMES otool NO_CACHE REQUIRED)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(_bioseq_dso_qualification_tool NAMES readelf NO_CACHE REQUIRED)
  else()
    message(FATAL_ERROR
      "production DSO qualification supports Darwin and Linux, got "
      "${CMAKE_SYSTEM_NAME}")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  set(_bioseq_dso "$<TARGET_FILE:${BIOSEQ_DSO_QUALIFY_BIOSEQ_TARGET}>")
  set(_cache_dso "$<TARGET_FILE:${BIOSEQ_DSO_QUALIFY_CACHE_TARGET}>")
  set(_receipt_root
    "${CMAKE_BINARY_DIR}/generated/production_dso_qualification")
  set(_receipt
    "${_receipt_root}/bioseq-production-dso-load-qualification.json")
  set(_qualification_script
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/qualify_production_extension_dsos.py")

  add_custom_command(
    OUTPUT "${_receipt}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_receipt_root}"
    COMMAND
      "${Python3_EXECUTABLE}"
      "${_qualification_script}"
      --platform "${CMAKE_SYSTEM_NAME}"
      --system-processor "${CMAKE_SYSTEM_PROCESSOR}"
      --inspection-tool "${_bioseq_dso_qualification_tool}"
      --bioseq-dso "${_bioseq_dso}"
      --cache-dso "${_cache_dso}"
      --build-directory "${CMAKE_BINARY_DIR}"
      --build-config "$<CONFIG>"
      --build-type "${CMAKE_BUILD_TYPE}"
      --generator "${CMAKE_GENERATOR}"
      --c-compiler "${CMAKE_C_COMPILER}"
      --c-compiler-id "${CMAKE_C_COMPILER_ID}"
      --c-compiler-version "${CMAKE_C_COMPILER_VERSION}"
      --cxx-compiler "${CMAKE_CXX_COMPILER}"
      --cxx-compiler-id "${CMAKE_CXX_COMPILER_ID}"
      --cxx-compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
      --cmake-cache "${CMAKE_BINARY_DIR}/CMakeCache.txt"
      --configure-input-stamp
        "${CMAKE_BINARY_DIR}/.bioseq-build-mcp-configure.json"
      --receipt "${_receipt}"
    DEPENDS
      "${BIOSEQ_DSO_QUALIFY_BIOSEQ_TARGET}"
      "${BIOSEQ_DSO_QUALIFY_CACHE_TARGET}"
      "${_bioseq_dso}"
      "${_cache_dso}"
      "${_bioseq_dso_qualification_tool}"
      "${CMAKE_BINARY_DIR}/CMakeCache.txt"
      "${CMAKE_BINARY_DIR}/.bioseq-build-mcp-configure.json"
      "${_qualification_script}"
      "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    COMMENT "Qualifying host-native production Bioseq/cache DSO loads"
    VERBATIM)
  add_custom_target(
    "${BIOSEQ_DSO_QUALIFY_TARGET}"
    DEPENDS "${_receipt}")
  set_target_properties(
    "${BIOSEQ_DSO_QUALIFY_TARGET}"
    PROPERTIES
      BIOSEQ_PRODUCTION_DSO_QUALIFICATION_RECEIPT "${_receipt}")
endfunction()
