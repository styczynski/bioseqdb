# bench_v10.cmake — wiring for the V10 bioseq aligner bench
# (`bioseq_bench`). Replaces the legacy AddCommonTargetX block in
# `main/bioseq/CMakeLists.txt` for the `bioseq_bench` target.
#
# Included from `main/bioseq/CMakeLists.txt` after the framework's
# `find_package(...)` calls have been resolved.

get_filename_component(BIOSEQ_DIR "${CMAKE_CURRENT_LIST_DIR}/../bioseq" ABSOLUTE)
set(BENCH_FRAMEWORK_DIR "${BIOSEQ_DIR}/../bench_framework")
set(BENCH_V10_DIR       "${CMAKE_CURRENT_LIST_DIR}")

# Module set: ONLY the bench-private modules. The whole Domain N core
# (alignment_hwy, common-minus-testing, sequences, pipeline, instrumentation)
# is delivered via `bioseq_modules_nopg` (DOC0269 Phase A).
set(BENCH_V10_MOD_FILES
  "${BENCH_FRAMEWORK_DIR}/index.cppm"
  # DOC0264 §10.1 — sequence_generator moved to
  # main/bioseq/backend/sequences/generation/sequence_generator.cppm and
  # is delivered via bioseq_modules_nopg below.
  "${BENCH_V10_DIR}/tool_minimap2/tool_minimap2.cppm"
  "${BENCH_V10_DIR}/tool_minimap2/alignment.cppm"
  "${BENCH_V10_DIR}/tool_minimap2/instr_shim.cppm")

# minimap2 C reference sources are linked alongside the bench TU.
file(GLOB_RECURSE BENCH_V10_C_SRC
  "${BENCH_V10_DIR}/tool_minimap2/minimap2/*.c"
  "${BENCH_V10_DIR}/tool_minimap2/minimap2/*.cpp")

AddCommonTargetX(
  bioseq_bench
  TYPE       EXEC
  CXX        26
  MAIN       "bench_v10_main\\.cpp"
  RELOCATABLE
  NAME       bioseq_bench
  COMMENT    "BioSeq Aligner benchmark harness (v10)"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config"
    "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/backend"
    "${BENCH_V10_DIR}/tool_minimap2"
    "${BENCH_V10_DIR}/tool_sneakysnake/SneakySnake"
    "${BIOSEQ_DIR}/../vendor/safestringlib/include"
    "${BIOSEQ_DIR}/../vendor/fmtlog"
    "${BIOSEQ_DIR}/../vendor/fmt"
    "${BIOSEQ_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    ftxui::component
    ftxui::dom
    ftxui::screen
    simdjson::simdjson
    nlohmann_json_schema_validator::validator
    TBB::tbb
    lfreist-hwinfo::hwinfo
    # DOC0345 Patch D — minimizer_dispatch_range now wires the Vulkan segment
    # sketch path into the target dispatch, so bioseq_modules_nopg's GPU runtime
    # (vulkan/shader_compiler) is referenced from bioseq_bench. Same glslang set
    # as bench_e2e.cmake / bioseq_aligner.
    Vulkan::Vulkan
    Vulkan::glslang
    spirv-cross-msl
    Vulkan::LayerSettings
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BENCH_V10_DIR}/bench_v10_main.cpp"
    "${BENCH_V10_DIR}/bench_prefilters.cpp"
    "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
    # Keep the upstream reference linkage deliberately narrow. The complete
    # worktree is vendored for provenance/data, but no driver, kthread, CUDA,
    # FPGA, or HLS source participates in bioseq_bench.
    "${BENCH_V10_DIR}/tool_sneakysnake/SneakySnake/SneakySnake.c"
    ${BENCH_V10_C_SRC}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BENCH_V10_DIR}"
    "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_V10_MOD_FILES}
  HWY 1.3.0)

target_compile_definitions(bioseq_bench
  PRIVATE
    "BIOSEQ_BENCH_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_bench\""
    "BIOSEQ_BENCH_DATA_DIR=\"${BENCH_V10_DIR}\""
    "BIOSEQ_BENCH_CONFIG_PATH=\"${BENCH_V10_DIR}/bench_v10_configuration.json\"")

# Kernel microbenchmarks are independent test roots, not production scheduler
# invocations. Give them the same generator-proven fixed table as unit tests so
# they never call an active-target facade or introduce a second selector.
set_source_files_properties(
  "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}"
  "${BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT}"
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS}
  PROPERTIES GENERATED TRUE)
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_BINDERS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_sources(bioseq_bench
  PRIVATE ${BIOSEQ_HIGHWAY_GENERATED_TEST_BINDERS}
  PRIVATE FILE_SET bioseq_highway_fixed_target_bench_modules
    TYPE CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES
      "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}"
      "${BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT}"
      ${BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS})
target_link_libraries(bioseq_bench PRIVATE bioseq_highway_target_policy)
add_dependencies(bioseq_bench bioseq_highway_target_modules)

if(APPLE)
    target_link_options(bioseq_bench PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
