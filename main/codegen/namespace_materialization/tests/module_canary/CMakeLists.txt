add_executable(bioseq_namespace_materialization_module_canary)

set(_bioseq_namespace_materialization_canary_dir
  "${CMAKE_CURRENT_LIST_DIR}")
set(_bioseq_namespace_materialization_canary_source
  "${_bioseq_namespace_materialization_canary_dir}/input.cppm")
set(_bioseq_namespace_materialization_canary_primary
  "${_bioseq_namespace_materialization_canary_dir}/primary.cppm")
set(_bioseq_namespace_materialization_canary_generated_root
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

bioseq_template_flatten_probe(
  NAME namespace_materialization_module_canary
  SOURCE "${_bioseq_namespace_materialization_canary_source}"
  OUTPUT_ROOT "${_bioseq_namespace_materialization_canary_generated_root}"
  OUTPUT_MODULE bioseq.codegen.namespace_materialization.canary
  PROFILE 2
  OUT_CONFIGURE_TOPOLOGY_MANIFEST
    _bioseq_namespace_materialization_canary_configure_topology
  OUT_TOPOLOGY_DIGEST
    _bioseq_namespace_materialization_canary_topology_digest
  OUT_ENABLED_ORDINALS
    _bioseq_namespace_materialization_canary_enabled_ordinals
  OUT_ENABLED_SOURCES
    _bioseq_namespace_materialization_canary_probed_sources)

bioseq_add_template_flatten_codegen(
  NAME namespace_materialization_module_canary
  SOURCE "${_bioseq_namespace_materialization_canary_source}"
  OUTPUT_ROOT "${_bioseq_namespace_materialization_canary_generated_root}"
  OUTPUT_MODULE bioseq.codegen.namespace_materialization.canary
  CONFIGURE_TOPOLOGY_MANIFEST
    "${_bioseq_namespace_materialization_canary_configure_topology}"
  EXPECTED_TOPOLOGY_DIGEST
    "${_bioseq_namespace_materialization_canary_topology_digest}"
  ENABLED_ORDINALS
    ${_bioseq_namespace_materialization_canary_enabled_ordinals}
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE "${_bioseq_namespace_materialization_canary_primary}"
  PCM_ROOT "${_bioseq_namespace_materialization_canary_generated_root}/pcm"
  OUT_API_PARTITION _bioseq_namespace_materialization_canary_api
  OUT_SELECTOR_PARTITION _bioseq_namespace_materialization_canary_selector
  OUT_IMPLEMENTATION_UNITS
    _bioseq_namespace_materialization_canary_instances
  OUT_GENERATION_TARGET _bioseq_namespace_materialization_canary_codegen
  OUT_ARTIFACT_MANIFEST
    _bioseq_namespace_materialization_canary_artifact_manifest)

if(NOT "${_bioseq_namespace_materialization_canary_probed_sources}"
    STREQUAL "${_bioseq_namespace_materialization_canary_instances}")
  message(FATAL_ERROR
    "namespace-materialization canary probe/generator source sets diverged")
endif()
list(LENGTH _bioseq_namespace_materialization_canary_instances
  _bioseq_namespace_materialization_canary_instance_count)
if(NOT _bioseq_namespace_materialization_canary_instance_count EQUAL 4)
  message(FATAL_ERROR
    "namespace-materialization canary must generate exactly four 2x2 units")
endif()

set(_bioseq_namespace_materialization_canary_implementations
  "${_bioseq_namespace_materialization_canary_api}"
  "${_bioseq_namespace_materialization_canary_selector}"
  ${_bioseq_namespace_materialization_canary_instances}
  "${_bioseq_namespace_materialization_canary_dir}/driver.cppm")

target_sources(bioseq_namespace_materialization_module_canary
  PRIVATE
    "${_bioseq_namespace_materialization_canary_dir}/main.cpp"
    ${_bioseq_namespace_materialization_canary_implementations}
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS "${_bioseq_namespace_materialization_canary_dir}"
    FILES "${_bioseq_namespace_materialization_canary_primary}")
add_dependencies(
  bioseq_namespace_materialization_module_canary
  "${_bioseq_namespace_materialization_canary_codegen}")

# A .cppm suffix normally denotes an interface to compiler drivers. These are
# intentionally private implementation partitions/units and must remain
# ordinary module-scanned C++ sources, matching generated production units.
set_source_files_properties(
  ${_bioseq_namespace_materialization_canary_implementations}
  PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
set_target_properties(bioseq_namespace_materialization_module_canary PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_SCAN_FOR_MODULES ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

if(BIOSEQ_HIGHWAY_GCC_CANARY)
  include(BioseqHighwayGccCanary)
  bioseq_add_highway_gcc_canary(
    TARGET bioseq_highway_gcc_canary
    GENERATION_TARGET
      "${_bioseq_namespace_materialization_canary_codegen}"
    GENERATED_ROOT
      "${_bioseq_namespace_materialization_canary_generated_root}"
    PRIMARY "${_bioseq_namespace_materialization_canary_primary}"
    DRIVER
      "${_bioseq_namespace_materialization_canary_dir}/driver.cppm"
    MAIN "${_bioseq_namespace_materialization_canary_dir}/main.cpp")
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTING)
  add_test(
    NAME bioseq_highway_gcc_canary_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/gcc-canary-contract"
      "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/test_highway_gcc_canary.cmake")
  set_tests_properties(
    bioseq_highway_gcc_canary_contract
    PROPERTIES LABELS "core;unit;cmake;highway;gcc")
endif()
