# Primary Bioseq alignment benchmark wiring.
#
# Included from `bioseq/CMakeLists.txt` after the framework's
# `find_package(...)` calls have been resolved.

include("${PROJECT_SOURCE_DIR}/cmake/bioseq_source_inventory.cmake")

set(BIOSEQ_REPOSITORY_ROOT "${PROJECT_SOURCE_DIR}")
set(BIOSEQ_DIR "${BIOSEQ_REPOSITORY_ROOT}/bioseq")
set(BIOSEQ_ALIGNMENT_BENCHMARK_FRAMEWORK_DIR
  "${BIOSEQ_REPOSITORY_ROOT}/bench_framework")
set(BIOSEQ_ALIGNMENT_BENCHMARK_DIR "${BIOSEQ_REPOSITORY_ROOT}/bench")

# Module set: only the benchmark-private modules. The whole Domain N core
# (alignment_hwy, common-minus-testing, sequences, pipeline, instrumentation)
# is delivered via `bioseq_modules_nopg`.
bioseq_collect_authored_sources(
  BIOSEQ_ALIGNMENT_BENCHMARK_MODULES
  BASE_DIRECTORY "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}"
  INCLUDE_PATTERNS
    "minimap2/*.cppm"
    "minimap2/alignment/*.cppm"
    "prefilters/*.cppm"
  EXCLUDE_PATHS "minimap2/alignment/main.cppm"
  EXPECTED_COUNT 4)

# minimap2 C reference sources are linked alongside the bench TU.
bioseq_collect_authored_sources(
  BIOSEQ_ALIGNMENT_BENCHMARK_MINIMAP2_SOURCES
  BASE_DIRECTORY
    "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}/tool_minimap2/minimap2"
  RECURSE
  INCLUDE_PATTERNS "*.c" "*.cpp"
  EXPECTED_COUNT 17)

bioseq_add_postgresql_extension(
  bioseq_benchmark
  TYPE       EXEC
  CXX        26
  MAIN       "main\\.cppm"
  RELOCATABLE
  NAME       bioseq_benchmark
  COMMENT    "BioSeq alignment benchmark harness"
  VERSION    ${INSTALL_VERSION}
  COMPONENT  ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}" "${BIOSEQ_DIR}/backend/pg/include"
    "${BIOSEQ_DIR}/backend"
    "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}/tool_minimap2"
    "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}/tool_sneakysnake/SneakySnake"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/safestringlib/include"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmtlog"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/fmt"
    "${BIOSEQ_REPOSITORY_ROOT}/vendor/seqan3/include"
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
    # The shared GPU runtime references the Vulkan shader compiler, so the
    # benchmark carries the same complete glslang linkage as the extension.
    Vulkan::Vulkan
    Vulkan::glslang
    spirv-cross-msl
    Vulkan::LayerSettings
    bioseq_bench_framework
    bioseq_bench_prefilter_algorithm_selection
    bioseq_bench_prefilter_metrics
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}/minimap2/alignment/main.cppm"
    # Keep the upstream reference linkage deliberately narrow. The complete
    # worktree is vendored for provenance/data, but no driver, kthread, CUDA,
    # FPGA, or HLS source participates in bioseq_benchmark.
    "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}/tool_sneakysnake/SneakySnake/SneakySnake.c"
    ${BIOSEQ_ALIGNMENT_BENCHMARK_MINIMAP2_SOURCES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${BIOSEQ_DIR}"
    "${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}"
    "${BIOSEQ_ALIGNMENT_BENCHMARK_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BIOSEQ_ALIGNMENT_BENCHMARK_MODULES}
  HWY 1.3.0)

target_compile_definitions(bioseq_benchmark
  PRIVATE
    "BIOSEQ_BENCH_REPORT_DIR=\"${BIOSEQ_REPOSITORY_ROOT}/reporting/bioseq_benchmark\""
    "BIOSEQ_BENCH_DATA_DIR=\"${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}\""
    "BIOSEQ_BENCH_CONFIG_PATH=\"${BIOSEQ_ALIGNMENT_BENCHMARK_DIR}/alignment_benchmark_configuration.json\"")

# Kernel microbenchmarks consume the same generated fixed-target provider as
# unit tests; the provider is compiled once by its reusable module owner.
target_link_libraries(bioseq_benchmark PRIVATE bioseq_highway_test_support)
add_dependencies(bioseq_benchmark bioseq_highway_target_modules)

if(APPLE)
    target_link_options(bioseq_benchmark PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
endif()
