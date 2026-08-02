add_executable(bioseq_namespace_materialization_module_canary)

set(_bioseq_namespace_materialization_canary_dir
  "${CMAKE_CURRENT_LIST_DIR}")
set(_bioseq_namespace_materialization_canary_implementations
  "${_bioseq_namespace_materialization_canary_dir}/api.cppm"
  "${_bioseq_namespace_materialization_canary_dir}/coordinate_0_helper.cppm"
  "${_bioseq_namespace_materialization_canary_dir}/coordinate_0_root.cppm"
  "${_bioseq_namespace_materialization_canary_dir}/coordinate_1_helper.cppm"
  "${_bioseq_namespace_materialization_canary_dir}/coordinate_1_root.cppm"
  "${_bioseq_namespace_materialization_canary_dir}/driver.cppm")

target_sources(bioseq_namespace_materialization_module_canary
  PRIVATE
    "${_bioseq_namespace_materialization_canary_dir}/main.cpp"
    ${_bioseq_namespace_materialization_canary_implementations}
  PRIVATE
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS "${_bioseq_namespace_materialization_canary_dir}"
    FILES "${_bioseq_namespace_materialization_canary_dir}/primary.cppm")

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
