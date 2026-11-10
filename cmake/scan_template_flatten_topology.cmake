cmake_minimum_required(VERSION 4.2)

# Shares configure's bounded extractor; it needs no compiler or host generator.
foreach(_required IN ITEMS BUILD_ROOT NAME SOURCE OUTPUT_ROOT OUTPUT_MODULE
    PROFILE EMISSION_POLICY STAMP)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "template-flatten topology scan requires ${_required}")
  endif()
endforeach()
set(CMAKE_BINARY_DIR "${BUILD_ROOT}")
include("${CMAKE_CURRENT_LIST_DIR}/bioseq_build_config_codegen.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/bioseq_template_flatten_codegen.cmake")
bioseq_template_flatten_probe(
  NAME "${NAME}" SOURCE "${SOURCE}" OUTPUT_ROOT "${OUTPUT_ROOT}"
  OUTPUT_MODULE "${OUTPUT_MODULE}" PROFILE "${PROFILE}"
  EMISSION_POLICY "${EMISSION_POLICY}"
  OUT_CONFIGURE_TOPOLOGY_MANIFEST _manifest OUT_TOPOLOGY_DIGEST _digest
  OUT_ENABLED_ORDINALS _ordinals OUT_ENABLED_SOURCES _sources)
file(TOUCH "${STAMP}")
