add_executable(bioseq_namespace_materialization_owner_canary)

set(_bioseq_namespace_materialization_owner_canary_dir
  "${CMAKE_CURRENT_LIST_DIR}")
set(_bioseq_namespace_materialization_owner_canary_source
  "${_bioseq_namespace_materialization_owner_canary_dir}/input.cppm")
set(_bioseq_namespace_materialization_owner_canary_primary
  "${_bioseq_namespace_materialization_owner_canary_dir}/primary.cppm")
set(_bioseq_namespace_materialization_owner_canary_generated_root
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

bioseq_template_flatten_probe(
  NAME namespace_materialization_owner_canary
  SOURCE "${_bioseq_namespace_materialization_owner_canary_source}"
  OUTPUT_ROOT
    "${_bioseq_namespace_materialization_owner_canary_generated_root}"
  OUTPUT_MODULE bioseq.codegen.namespace_materialization.owner_canary
  PROFILE all_coordinates
  EMISSION_POLICY implementation_owners
  OUT_CONFIGURE_TOPOLOGY_MANIFEST
    _bioseq_namespace_materialization_owner_canary_configure_topology
  OUT_TOPOLOGY_DIGEST
    _bioseq_namespace_materialization_owner_canary_topology_digest
  OUT_ENABLED_ORDINALS
    _bioseq_namespace_materialization_owner_canary_enabled_ordinals
  OUT_ENABLED_SOURCES
    _bioseq_namespace_materialization_owner_canary_probed_sources)

if(NOT "${_bioseq_namespace_materialization_owner_canary_enabled_ordinals}"
    STREQUAL "0;1;2;3")
  message(FATAL_ERROR
    "namespace-materialization owner canary must enable ordinals 0,1,2,3")
endif()

bioseq_add_template_flatten_codegen(
  NAME namespace_materialization_owner_canary
  SOURCE "${_bioseq_namespace_materialization_owner_canary_source}"
  OUTPUT_ROOT
    "${_bioseq_namespace_materialization_owner_canary_generated_root}"
  OUTPUT_MODULE bioseq.codegen.namespace_materialization.owner_canary
  EMISSION_POLICY implementation_owners
  CONFIGURE_TOPOLOGY_MANIFEST
    "${_bioseq_namespace_materialization_owner_canary_configure_topology}"
  EXPECTED_TOPOLOGY_DIGEST
    "${_bioseq_namespace_materialization_owner_canary_topology_digest}"
  ENABLED_ORDINALS
    ${_bioseq_namespace_materialization_owner_canary_enabled_ordinals}
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${_bioseq_namespace_materialization_owner_canary_primary}"
  PCM_ROOT
    "${_bioseq_namespace_materialization_owner_canary_generated_root}/pcm"
  OUT_IMPLEMENTATION_UNITS
    _bioseq_namespace_materialization_owner_canary_instances
  OUT_GENERATION_TARGET
    _bioseq_namespace_materialization_owner_canary_codegen
  OUT_ARTIFACT_MANIFEST
    _bioseq_namespace_materialization_owner_canary_artifact_manifest)

if(NOT "${_bioseq_namespace_materialization_owner_canary_probed_sources}"
    STREQUAL "${_bioseq_namespace_materialization_owner_canary_instances}")
  message(FATAL_ERROR
    "namespace-materialization owner canary probe/generator source sets diverged")
endif()
list(LENGTH _bioseq_namespace_materialization_owner_canary_instances
  _bioseq_namespace_materialization_owner_canary_instance_count)
if(NOT _bioseq_namespace_materialization_owner_canary_instance_count EQUAL 4)
  message(FATAL_ERROR
    "namespace-materialization owner canary must generate exactly four units")
endif()
foreach(_bioseq_namespace_materialization_owner_canary_instance IN LISTS
    _bioseq_namespace_materialization_owner_canary_instances)
  get_filename_component(
    _bioseq_namespace_materialization_owner_canary_basename
    "${_bioseq_namespace_materialization_owner_canary_instance}" NAME)
  if(NOT _bioseq_namespace_materialization_owner_canary_basename MATCHES
      "^instance_[0-3]\\.cppm$")
    message(FATAL_ERROR
      "namespace-materialization owner canary emitted a non-owner source: "
      "${_bioseq_namespace_materialization_owner_canary_basename}")
  endif()
endforeach()

target_sources(bioseq_namespace_materialization_owner_canary
  PRIVATE
    "${_bioseq_namespace_materialization_owner_canary_dir}/main.cpp"
    ${_bioseq_namespace_materialization_owner_canary_instances}
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS
      "${_bioseq_namespace_materialization_owner_canary_dir}"
    FILES
      "${_bioseq_namespace_materialization_owner_canary_primary}")
add_dependencies(
  bioseq_namespace_materialization_owner_canary
  "${_bioseq_namespace_materialization_owner_canary_codegen}")

set_source_files_properties(
  ${_bioseq_namespace_materialization_owner_canary_instances}
  PROPERTIES COMPILE_OPTIONS "-x;c++" CXX_SCAN_FOR_MODULES ON)
set_target_properties(
  bioseq_namespace_materialization_owner_canary
  PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_SCAN_FOR_MODULES ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)
